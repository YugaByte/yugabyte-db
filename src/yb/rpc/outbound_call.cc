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

#include <algorithm>
#include <string>
#include <mutex>
#include <vector>

#include <boost/functional/hash.hpp>

#include <gflags/gflags.h>

#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/walltime.h"

#include "yb/rpc/connection.h"
#include "yb/rpc/constants.h"
#include "yb/rpc/outbound_call.h"
#include "yb/rpc/rpc_controller.h"
#include "yb/rpc/rpc_introspection.pb.h"
#include "yb/rpc/rpc_metrics.h"
#include "yb/rpc/serialization.h"

#include "yb/util/concurrent_value.h"
#include "yb/util/flag_tags.h"
#include "yb/util/memory/memory.h"
#include "yb/util/pb_util.h"
#include "yb/util/trace.h"
#include "yb/util/tsan_util.h"

METRIC_DEFINE_histogram(
    server, handler_latency_outbound_call_queue_time, "Time taken to queue the request ",
    yb::MetricUnit::kMicroseconds, "Microseconds spent to queue the request to the reactor",
    60000000LU, 2);
METRIC_DEFINE_histogram(
    server, handler_latency_outbound_call_send_time, "Time taken to send the request ",
    yb::MetricUnit::kMicroseconds, "Microseconds spent to queue and write the request to the wire",
    60000000LU, 2);
METRIC_DEFINE_histogram(
    server, handler_latency_outbound_call_time_to_response, "Time taken to get the response ",
    yb::MetricUnit::kMicroseconds,
    "Microseconds spent to send the request and get a response on the wire", 60000000LU, 2);

// 100M cycles should be about 50ms on a 2Ghz box. This should be high
// enough that involuntary context switches don't trigger it, but low enough
// that any serious blocking behavior on the reactor would.
DEFINE_int64(
    rpc_callback_max_cycles, 100 * 1000 * 1000 * yb::kTimeMultiplier,
    "The maximum number of cycles for which an RPC callback "
    "should be allowed to run without emitting a warning."
    " (Advanced debugging option)");
TAG_FLAG(rpc_callback_max_cycles, advanced);
TAG_FLAG(rpc_callback_max_cycles, runtime);
DECLARE_bool(rpc_dump_all_traces);

namespace yb {
namespace rpc {

using strings::Substitute;
using google::protobuf::Message;
using google::protobuf::io::CodedOutputStream;

OutboundCallMetrics::OutboundCallMetrics(const scoped_refptr<MetricEntity>& entity)
    : queue_time(METRIC_handler_latency_outbound_call_queue_time.Instantiate(entity)),
      send_time(METRIC_handler_latency_outbound_call_send_time.Instantiate(entity)),
      time_to_response(METRIC_handler_latency_outbound_call_time_to_response.Instantiate(entity)) {
}

namespace {

std::atomic<int32_t> call_id_ = {0};

int32_t NextCallId() {
  for (;;) {
    auto result = call_id_.fetch_add(1, std::memory_order_acquire);
    ++result;
    if (result > 0) {
      return result;
    }
    // When call id overflows, we reset it to zero.
    call_id_.compare_exchange_weak(result, 0);
  }
}

class RemoteMethodsCache {
 public:
  RemoteMethodPool* Find(const RemoteMethod& method) {
    {
      auto cache = concurrent_cache_.get();
      auto it = cache->find(method);
      if (it != cache->end()) {
        return it->second;
      }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(method);
    if (it == cache_.end()) {
      auto result = std::make_shared<RemoteMethodPool>([method]() -> RemoteMethodPB* {
        auto remote_method = new RemoteMethodPB();
        method.ToPB(remote_method);
        return remote_method;
      });
      cache_.emplace(method, result);
      PtrCache new_ptr_cache;
      for (const auto& p : cache_) {
        new_ptr_cache.emplace(p.first, p.second.get());
      }
      concurrent_cache_.Set(std::move(new_ptr_cache));
      return result.get();
    }

    return it->second.get();
  }

  static RemoteMethodsCache& Instance() {
    static RemoteMethodsCache instance;
    return instance;
  }

 private:
  typedef std::unordered_map<
      RemoteMethod, std::shared_ptr<RemoteMethodPool>, RemoteMethodHash> Cache;
  typedef std::unordered_map<RemoteMethod, RemoteMethodPool*, RemoteMethodHash> PtrCache;
  std::mutex mutex_;
  Cache cache_;
  ConcurrentValue<PtrCache> concurrent_cache_;
};

const std::string kEmptyString;

} // namespace

void InvokeCallbackTask::Run() {
  CHECK_NOTNULL(call_.get());
  call_->InvokeCallbackSync();
}

void InvokeCallbackTask::Done(const Status& status) {
  CHECK_NOTNULL(call_.get());
  if (!status.ok()) {
    LOG(WARNING) << Format(
        "Failed to schedule invoking callback on response for request $0 to $1: $2",
        call_->remote_method(), call_->hostname(), status);
    call_->SetThreadPoolFailure(status);
    call_->InvokeCallbackSync();
  }
  // Clear the call, since it holds OutboundCall object.
  call_ = nullptr;
}

///
/// OutboundCall
///

OutboundCall::OutboundCall(const RemoteMethod* remote_method,
                           const std::shared_ptr<OutboundCallMetrics>& outbound_call_metrics,
                           google::protobuf::Message* response_storage,
                           RpcController* controller,
                           RpcMetrics* rpc_metrics,
                           ResponseCallback callback,
                           ThreadPool* callback_thread_pool)
    : hostname_(&kEmptyString),
      start_(MonoTime::Now()),
      controller_(DCHECK_NOTNULL(controller)),
      response_(DCHECK_NOTNULL(response_storage)),
      call_id_(NextCallId()),
      remote_method_(remote_method),
      callback_(std::move(callback)),
      callback_thread_pool_(callback_thread_pool),
      trace_(new Trace),
      outbound_call_metrics_(outbound_call_metrics),
      remote_method_pool_(RemoteMethodsCache::Instance().Find(*remote_method_)),
      rpc_metrics_(rpc_metrics) {
  // Avoid expensive conn_id.ToString() in production.
  TRACE_TO_WITH_TIME(trace_, start_, "Outbound Call initiated.");

  if (Trace::CurrentTrace()) {
    Trace::CurrentTrace()->AddChildTrace(trace_.get());
  }

  DVLOG(4) << "OutboundCall " << this << " constructed with state_: " << StateName(state_)
           << " and RPC timeout: "
           << (controller_->timeout().Initialized() ? controller_->timeout().ToString() : "none");

  IncrementCounter(rpc_metrics_->outbound_calls_created);
  IncrementGauge(rpc_metrics_->outbound_calls_alive);
}

OutboundCall::~OutboundCall() {
  DCHECK(IsFinished());
  DVLOG(4) << "OutboundCall " << this << " destroyed with state_: " << StateName(state_);

  if (PREDICT_FALSE(FLAGS_rpc_dump_all_traces)) {
    LOG(INFO) << ToString() << " took "
              << MonoTime::Now().GetDeltaSince(start_).ToMicroseconds()
              << "us. Trace:";
    trace_->Dump(&LOG(INFO), true);
  }

  DecrementGauge(rpc_metrics_->outbound_calls_alive);
}

void OutboundCall::NotifyTransferred(const Status& status, Connection* conn) {
  if (status.ok()) {
    // Even when call is already finished (timed out) we should notify connection that it was sent
    // because it should expect response with appropriate id.
    conn->CallSent(shared_from(this));
  }

  if (IsFinished()) {
    LOG_IF_WITH_PREFIX(DFATAL, !IsTimedOut())
        << "Transferred call is in wrong state: " << state_.load(std::memory_order_acquire);
  } else if (status.ok()) {
    SetSent();
  } else {
    VLOG_WITH_PREFIX(1) << "Connection torn down: " << status;
    SetFailed(status);
  }
}

void OutboundCall::Serialize(boost::container::small_vector_base<RefCntBuffer>* output) {
  output->push_back(std::move(buffer_));
  buffer_consumption_ = ScopedTrackedConsumption();
}

Status OutboundCall::SetRequestParam(
    const Message& message, const MemTrackerPtr& mem_tracker) {
  using serialization::SerializeHeader;
  using serialization::SerializeMessage;

  size_t message_size = 0;
  auto status = SerializeMessage(message,
                                 /* param_buf */ nullptr,
                                 /* additional_size */ 0,
                                 /* use_cached_size */ false,
                                 /* offset */ 0,
                                 &message_size);
  if (!status.ok()) {
    return status;
  }
  size_t header_size = 0;

  RequestHeader header;
  InitHeader(&header);
  status = SerializeHeader(header, message_size, &buffer_, message_size, &header_size);
  remote_method_pool_->Release(header.release_remote_method());
  if (!status.ok()) {
    return status;
  }

  if (mem_tracker) {
    buffer_consumption_ = ScopedTrackedConsumption(mem_tracker, buffer_.size());
  }

  return SerializeMessage(message,
                          &buffer_,
                          /* additional_size */ 0,
                          /* use_cached_size */ true,
                          header_size);
}

Status OutboundCall::status() const {
  std::lock_guard<simple_spinlock> l(lock_);
  return status_;
}

const ErrorStatusPB* OutboundCall::error_pb() const {
  std::lock_guard<simple_spinlock> l(lock_);
  return error_pb_.get();
}


string OutboundCall::StateName(State state) {
  return RpcCallState_Name(state);
}

OutboundCall::State OutboundCall::state() const {
  return state_.load(std::memory_order_acquire);
}

bool FinishedState(RpcCallState state) {
  switch (state) {
    case READY:
    case ON_OUTBOUND_QUEUE:
    case SENT:
      return false;
    case TIMED_OUT:
    case FINISHED_ERROR:
    case FINISHED_SUCCESS:
      return true;
  }
  LOG(FATAL) << "Unknown call state: " << state;
  return false;
}

bool ValidStateTransition(RpcCallState old_state, RpcCallState new_state) {
  switch (new_state) {
    case ON_OUTBOUND_QUEUE:
      return old_state == READY;
    case SENT:
      return old_state == ON_OUTBOUND_QUEUE;
    case TIMED_OUT:
      return old_state == SENT || old_state == ON_OUTBOUND_QUEUE;
    case FINISHED_SUCCESS:
      return old_state == SENT;
    case FINISHED_ERROR:
      return old_state == SENT || old_state == ON_OUTBOUND_QUEUE || old_state == READY;
    default:
      // No sanity checks for others.
      return true;
  }
}

bool OutboundCall::SetState(State new_state) {
  auto old_state = state_.load(std::memory_order_acquire);
  // Sanity check state transitions.
  DVLOG(3) << "OutboundCall " << this << " (" << ToString() << ") switching from " <<
    StateName(old_state) << " to " << StateName(new_state);
  for (;;) {
    if (FinishedState(old_state)) {
      VLOG(1) << "Call already finished: " << RpcCallState_Name(old_state) << ", new state: "
              << RpcCallState_Name(new_state);
      return false;
    }
    if (!ValidStateTransition(old_state, new_state)) {
      LOG(DFATAL)
          << "Invalid call state transition: " << RpcCallState_Name(old_state) << " => "
          << RpcCallState_Name(new_state);
      return false;
    }
    if (state_.compare_exchange_weak(old_state, new_state, std::memory_order_acq_rel)) {
      return true;
    }
  }
}

void OutboundCall::InvokeCallback() {
  if (callback_thread_pool_) {
    callback_task_.SetOutboundCall(shared_from(this));
    callback_thread_pool_->Enqueue(&callback_task_);
    TRACE_TO(trace_, "Callback called asynchronously.");
  } else {
    InvokeCallbackSync();
    TRACE_TO(trace_, "Callback called.");
  }
}

void OutboundCall::InvokeCallbackSync() {
  if (!callback_) {
    LOG(DFATAL) << "Callback has been already invoked.";
    return;
  }

  int64_t start_cycles = CycleClock::Now();
  callback_();
  // Clear the callback, since it may be holding onto reference counts
  // via bound parameters. We do this inside the timer because it's possible
  // the user has naughty destructors that block, and we want to account for that
  // time here if they happen to run on this thread.
  callback_ = nullptr;
  int64_t end_cycles = CycleClock::Now();
  int64_t wait_cycles = end_cycles - start_cycles;
  if (PREDICT_FALSE(wait_cycles > FLAGS_rpc_callback_max_cycles)) {
    auto time_spent = MonoDelta::FromSeconds(
        static_cast<double>(wait_cycles) / base::CyclesPerSecond());

    LOG(WARNING) << "RPC callback for " << ToString() << " took " << time_spent;
  }

  // Could be destroyed during callback. So reset it.
  controller_ = nullptr;
  response_ = nullptr;
}

void OutboundCall::SetResponse(CallResponse&& resp) {
  DCHECK(!IsFinished());

  auto now = MonoTime::Now();
  TRACE_TO_WITH_TIME(trace_, now, "Response received.");
  // Track time taken to be responded.

  if (outbound_call_metrics_) {
    outbound_call_metrics_->time_to_response->Increment(now.GetDeltaSince(start_).ToMicroseconds());
  }
  call_response_ = std::move(resp);
  Slice r(call_response_.serialized_response());

  if (call_response_.is_success()) {
    // TODO: here we're deserializing the call response within the reactor thread,
    // which isn't great, since it would block processing of other RPCs in parallel.
    // Should look into a way to avoid this.
    if (!pb_util::ParseFromArray(response_, r.data(), r.size()).IsOk()) {
      SetFailed(STATUS(IOError, "Invalid response, missing fields",
                                response_->InitializationErrorString()));
      return;
    }
    if (SetState(FINISHED_SUCCESS)) {
      InvokeCallback();
    } else {
      LOG(DFATAL) << "Success of already finished call: "
                  << RpcCallState_Name(state_.load(std::memory_order_acquire));
    }
  } else {
    // Error
    auto err = std::make_unique<ErrorStatusPB>();
    if (!pb_util::ParseFromArray(err.get(), r.data(), r.size()).IsOk()) {
      SetFailed(STATUS(IOError, "Was an RPC error but could not parse error response",
                                err->InitializationErrorString()));
      return;
    }
    auto status = STATUS(RemoteError, err->message());
    SetFailed(status, std::move(err));
  }
}

void OutboundCall::SetQueued() {
  auto end_time = MonoTime::Now();
  // Track time taken to be queued.
  if (outbound_call_metrics_) {
    outbound_call_metrics_->queue_time->Increment(end_time.GetDeltaSince(start_).ToMicroseconds());
  }
  SetState(ON_OUTBOUND_QUEUE);
  TRACE_TO_WITH_TIME(trace_, end_time, "Queued.");
}

void OutboundCall::SetSent() {
  auto end_time = MonoTime::Now();
  // Track time taken to be sent
  if (outbound_call_metrics_) {
    outbound_call_metrics_->send_time->Increment(end_time.GetDeltaSince(start_).ToMicroseconds());
  }
  SetState(SENT);
  TRACE_TO_WITH_TIME(trace_, end_time, "Call Sent.");
}

void OutboundCall::SetFinished() {
  DCHECK(!IsFinished());

  // Track time taken to be responded.
  if (outbound_call_metrics_) {
    outbound_call_metrics_->time_to_response->Increment(
        MonoTime::Now().GetDeltaSince(start_).ToMicroseconds());
  }
  if (SetState(FINISHED_SUCCESS)) {
    InvokeCallback();
  }
  TRACE_TO(trace_, "Callback called.");
}

void OutboundCall::SetFailed(const Status &status, std::unique_ptr<ErrorStatusPB> err_pb) {
  DCHECK(!IsFinished());

  TRACE_TO(trace_, "Call Failed.");
  bool invoke_callback;
  {
    std::lock_guard<simple_spinlock> l(lock_);
    status_ = status;
    if (status_.IsRemoteError()) {
      CHECK(err_pb);
      error_pb_ = std::move(err_pb);
    } else {
      CHECK(!err_pb);
    }
    invoke_callback = SetState(FINISHED_ERROR);
  }
  if (invoke_callback) {
    InvokeCallback();
  }
}

void OutboundCall::SetTimedOut() {
  DCHECK(!IsFinished());

  TRACE_TO(trace_, "Call TimedOut.");
  bool invoke_callback;
  {
    auto status = STATUS_FORMAT(
        TimedOut,
        "$0 RPC (request call id $3) to $1 timed out after $2",
        remote_method_->method_name(),
        conn_id_.remote(),
        controller_->timeout(),
        call_id_);
    std::lock_guard<simple_spinlock> l(lock_);
    status_ = std::move(status);
    invoke_callback = SetState(TIMED_OUT);
  }
  if (invoke_callback) {
    InvokeCallback();
  }
}

bool OutboundCall::IsTimedOut() const {
  return state_.load(std::memory_order_acquire) == TIMED_OUT;
}

bool OutboundCall::IsFinished() const {
  return FinishedState(state_.load(std::memory_order_acquire));
}

Result<Slice> OutboundCall::GetSidecar(int idx) const {
  return call_response_.GetSidecar(idx);
}

string OutboundCall::ToString() const {
  return Format("RPC call $0 -> $1 , state=$2.", *remote_method_, conn_id_, StateName(state_));
}

bool OutboundCall::DumpPB(const DumpRunningRpcsRequestPB& req,
                          RpcCallInProgressPB* resp) {
  std::lock_guard<simple_spinlock> l(lock_);
  auto state_value = state();
  if (!req.dump_timed_out() && state_value == RpcCallState::TIMED_OUT) {
    return false;
  }
  InitHeader(resp->mutable_header());
  resp->set_elapsed_millis(MonoTime::Now().GetDeltaSince(start_).ToMilliseconds());
  resp->set_state(state_value);
  if (req.include_traces() && trace_) {
    resp->set_trace_buffer(trace_->DumpToString(true));
  }
  return true;
}

std::string OutboundCall::LogPrefix() const {
  return Format("{ OutboundCall@$0 } ", this);
}

void OutboundCall::InitHeader(RequestHeader* header) {
  header->set_call_id(call_id_);

  if (!IsFinished()) {
    MonoDelta timeout = controller_->timeout();
    if (timeout.Initialized()) {
      header->set_timeout_millis(timeout.ToMilliseconds());
    }
  }
  header->set_allocated_remote_method(remote_method_pool_->Take());
}

///
/// ConnectionId
///

string ConnectionId::ToString() const {
  return Format("{ remote: $0 idx: $1 protocol: $2 }", remote_, idx_, protocol_);
}

size_t ConnectionId::HashCode() const {
  size_t seed = 0;
  boost::hash_combine(seed, hash_value(remote_));
  boost::hash_combine(seed, idx_);
  boost::hash_combine(seed, protocol_);
  return seed;
}

size_t ConnectionIdHash::operator() (const ConnectionId& conn_id) const {
  return conn_id.HashCode();
}

///
/// CallResponse
///

CallResponse::CallResponse()
    : parsed_(false) {
}

Result<Slice> CallResponse::GetSidecar(int idx) const {
  DCHECK(parsed_);
  if (idx < 0 || idx + 1 >= sidecar_bounds_.size()) {
    return STATUS_FORMAT(InvalidArgument,
        "Index $0 does not reference a valid sidecar", idx);
  }
  return Slice(sidecar_bounds_[idx], sidecar_bounds_[idx + 1]);
}

Status CallResponse::ParseFrom(CallData* call_data) {
  CHECK(!parsed_);
  Slice entire_message;

  response_data_ = std::move(*call_data);
  Slice source(response_data_.data(), response_data_.size());
  RETURN_NOT_OK(serialization::ParseYBMessage(source, &header_, &entire_message));

  // Use information from header to extract the payload slices.
  const size_t sidecars = header_.sidecar_offsets_size();

  if (sidecars > 0) {
    serialized_response_ = Slice(entire_message.data(),
                                 header_.sidecar_offsets(0));
    sidecar_bounds_.reserve(sidecars + 1);

    uint32_t prev_offset = 0;
    for (auto offset : header_.sidecar_offsets()) {
      if (offset > entire_message.size() || offset < prev_offset) {
        return STATUS_FORMAT(
            Corruption,
            "Invalid sidecar offsets; sidecar apparently starts at $0,"
            " ends at $1, but the entire message has length $2",
            prev_offset, offset, entire_message.size());
      }
      sidecar_bounds_.push_back(entire_message.data() + offset);
      prev_offset = offset;
    }
    sidecar_bounds_.emplace_back(entire_message.end());
  } else {
    serialized_response_ = entire_message;
  }

  parsed_ = true;
  return Status::OK();
}

}  // namespace rpc
}  // namespace yb
