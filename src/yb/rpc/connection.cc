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

#include "yb/rpc/connection.h"

#include <iostream>
#include <thread>
#include <utility>

#include "yb/gutil/map-util.h"
#include "yb/gutil/strings/human_readable.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/util/enums.h"

#include "yb/rpc/rpc_introspection.pb.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/reactor.h"
#include "yb/rpc/growable_buffer.h"
#include "yb/rpc/rpc_controller.h"
#include "yb/rpc/rpc_metrics.h"

#include "yb/util/string_util.h"
#include "yb/util/trace.h"
#include "yb/util/tsan_util.h"

using namespace std::literals;
using namespace std::placeholders;
using std::shared_ptr;
using std::vector;
using strings::Substitute;

DEFINE_uint64(rpc_connection_timeout_ms, yb::NonTsanVsTsan(15000, 30000),
    "Timeout for RPC connection operations");

METRIC_DEFINE_histogram_with_percentiles(
    server, handler_latency_outbound_transfer, "Time taken to transfer the response ",
    yb::MetricUnit::kMicroseconds, "Microseconds spent to queue and write the response to the wire",
    60000000LU, 2);

namespace yb {
namespace rpc {

///
/// Connection
///
Connection::Connection(Reactor* reactor,
                       std::unique_ptr<Stream> stream,
                       Direction direction,
                       RpcMetrics* rpc_metrics,
                       std::unique_ptr<ConnectionContext> context)
    : reactor_(reactor),
      stream_(std::move(stream)),
      direction_(direction),
      last_activity_time_(CoarseMonoClock::Now()),
      rpc_metrics_(rpc_metrics),
      context_(std::move(context)) {
  const auto metric_entity = reactor->messenger()->metric_entity();
  handler_latency_outbound_transfer_ = metric_entity ?
      METRIC_handler_latency_outbound_transfer.Instantiate(metric_entity) : nullptr;
  IncrementCounter(rpc_metrics_->connections_created);
  IncrementGauge(rpc_metrics_->connections_alive);
}

Connection::~Connection() {
  DecrementGauge(rpc_metrics_->connections_alive);
}

void UpdateIdleReason(const char* message, bool* result, std::string* reason) {
  *result = false;
  if (reason) {
    AppendWithSeparator(message, reason);
  }
}

bool Connection::Idle(std::string* reason_not_idle) const {
  DCHECK(reactor_->IsCurrentThread());

  bool result = stream_->Idle(reason_not_idle);

  // Connection is not idle if calls are waiting for a response.
  if (!awaiting_response_.empty()) {
    UpdateIdleReason("awaiting response", &result, reason_not_idle);
  }

  // Check upstream logic (i.e. processing calls, etc.)
  return context_->Idle(reason_not_idle) && result;
}

std::string Connection::ReasonNotIdle() const {
  std::string reason;
  Idle(&reason);
  return reason;
}

void Connection::Shutdown(const Status& status) {
  DCHECK(reactor_->IsCurrentThread());

  {
    std::lock_guard<simple_spinlock> lock(outbound_data_queue_lock_);
    outbound_data_being_processed_.swap(outbound_data_to_process_);
    shutdown_status_ = status;
  }

  // Clear any calls which have been sent and were awaiting a response.
  for (auto& v : awaiting_response_) {
    if (v.second) {
      v.second->SetFailed(status);
    }
  }
  awaiting_response_.clear();

  for (auto& call : outbound_data_being_processed_) {
    call->Transferred(status, this);
  }
  outbound_data_being_processed_.clear();

  context_->Shutdown(status);

  stream_->Shutdown(status);
  timer_.Shutdown();

  // TODO(bogdan): re-enable once we decide how to control verbose logs better...
  // LOG_WITH_PREFIX(INFO) << "Connection::Shutdown completed, status: " << status;
}

void Connection::OutboundQueued() {
  DCHECK(reactor_->IsCurrentThread());

  auto status = stream_->TryWrite();
  if (!status.ok()) {
    VLOG_WITH_PREFIX(1) << "Write failed: " << status;
    auto scheduled = reactor_->ScheduleReactorTask(
        MakeFunctorReactorTask(
            std::bind(&Reactor::DestroyConnection, reactor_, this, status),
            shared_from_this(), SOURCE_LOCATION()));
    LOG_IF_WITH_PREFIX(WARNING, !scheduled) << "Failed to schedule destroy";
  }
}

void Connection::HandleTimeout(ev::timer& watcher, int revents) {  // NOLINT
  DCHECK(reactor_->IsCurrentThread());
  DVLOG_WITH_PREFIX(5) << "Connection::HandleTimeout revents: " << revents
                       << " connected: " << stream_->IsConnected();

  if (EV_ERROR & revents) {
    LOG_WITH_PREFIX(WARNING) << "Got an error in handle timeout";
    return;
  }

  auto now = CoarseMonoClock::Now();

  CoarseTimePoint deadline = CoarseTimePoint::max();
  if (!stream_->IsConnected()) {
    const MonoDelta timeout = FLAGS_rpc_connection_timeout_ms * 1ms;
    deadline = last_activity_time_ + timeout;
    DVLOG_WITH_PREFIX(5) << Format("now: $0, deadline: $1, timeout: $2", now, deadline, timeout);
    if (now > deadline) {
      auto passed = reactor_->cur_time() - last_activity_time_;
      reactor_->DestroyConnection(
          this,
          STATUS_FORMAT(NetworkError, "Connect timeout, passed: $0, timeout: $1", passed, timeout));
      return;
    }
  }

  while (!expiration_queue_.empty() && expiration_queue_.top().time <= now) {
    auto& top = expiration_queue_.top();
    auto call = top.call.lock();
    auto handle = top.handle;
    expiration_queue_.pop();
    if (call && !call->IsFinished()) {
      call->SetTimedOut();
      if (handle != std::numeric_limits<size_t>::max()) {
        stream_->Cancelled(handle);
      }
      auto i = awaiting_response_.find(call->call_id());
      if (i != awaiting_response_.end()) {
        i->second.reset();
      }
    }
  }

  if (!expiration_queue_.empty()) {
    deadline = std::min(deadline, expiration_queue_.top().time);
  }

  if (deadline != CoarseTimePoint::max()) {
    timer_.Start(deadline - now);
  }
}

void Connection::QueueOutboundCall(const OutboundCallPtr& call) {
  DCHECK(call);
  DCHECK_EQ(direction_, Direction::CLIENT);

  auto handle = DoQueueOutboundData(call, true);

  // Set up the timeout timer.
  const MonoDelta& timeout = call->controller()->timeout();
  if (timeout.Initialized()) {
    auto expires_at = CoarseMonoClock::Now() + timeout.ToSteadyDuration();
    auto reschedule = expiration_queue_.empty() || expiration_queue_.top().time > expires_at;
    expiration_queue_.push({expires_at, call, handle});
    if (reschedule && (stream_->IsConnected() ||
                       expires_at < last_activity_time_ + FLAGS_rpc_connection_timeout_ms * 1ms)) {
      timer_.Start(timeout.ToSteadyDuration());
    }
  }

  call->SetQueued();
}

size_t Connection::DoQueueOutboundData(OutboundDataPtr outbound_data, bool batch) {
  DCHECK(reactor_->IsCurrentThread());
  DVLOG_WITH_PREFIX(4) << "Connection::DoQueueOutboundData: " << AsString(outbound_data);

  if (!shutdown_status_.ok()) {
    YB_LOG_EVERY_N_SECS(INFO, 5) << "Connection::DoQueueOutboundData data: "
                                 << AsString(outbound_data) << " shutdown_status_: "
                                 << shutdown_status_;
    outbound_data->Transferred(shutdown_status_, this);
    return std::numeric_limits<size_t>::max();
  }

  // If the connection is torn down, then the QueueOutbound() call that
  // eventually runs in the reactor thread will take care of calling
  // ResponseTransferCallbacks::NotifyTransferAborted.

  // Check before and after calling Send. Before to reset state, if we
  // were over the limit; but are now back in good standing. After, to
  // check if we are now over the limit.
  Status s = context_->ReportPendingWriteBytes(stream_->GetPendingWriteBytes());
  if (!s.ok()) {
    Shutdown(s);
    return std::numeric_limits<size_t>::max();
  }
  auto result = stream_->Send(std::move(outbound_data));
  if (!result.ok()) {
    Shutdown(result.status());
    return std::numeric_limits<size_t>::max();
  }
  s = context_->ReportPendingWriteBytes(stream_->GetPendingWriteBytes());
  if (!s.ok()) {
    Shutdown(s);
    return std::numeric_limits<size_t>::max();
  }

  if (!batch) {
    OutboundQueued();
  }

  return *result;
}

void Connection::ParseReceived() {
  stream_->ParseReceived();
}

Result<ProcessDataResult> Connection::ProcessReceived(
    const IoVecs& data, ReadBufferFull read_buffer_full) {
  auto result = context_->ProcessCalls(shared_from_this(), data, read_buffer_full);
  VLOG_WITH_PREFIX(4) << "context_->ProcessCalls result: " << AsString(result);
  if (PREDICT_FALSE(!result.ok())) {
    LOG_WITH_PREFIX(WARNING) << "Command sequence failure: " << result.status();
    return result;
  }

  if (!result->consumed && read_buffer_full && context_->Idle()) {
    return STATUS_FORMAT(
        InvalidArgument, "Command is greater than read buffer, exist data: $0",
        IoVecsFullSize(data));
  }

  return result;
}

Status Connection::HandleCallResponse(CallData* call_data) {
  DCHECK(reactor_->IsCurrentThread());
  CallResponse resp;
  RETURN_NOT_OK(resp.ParseFrom(call_data));

  ++responded_call_count_;
  auto awaiting = awaiting_response_.find(resp.call_id());
  if (awaiting == awaiting_response_.end()) {
    LOG_WITH_PREFIX(DFATAL) << "Got a response for call id " << resp.call_id() << " which "
                            << "was not pending! Ignoring.";
    return Status::OK();
  }
  auto call = awaiting->second;
  awaiting_response_.erase(awaiting);

  if (PREDICT_FALSE(!call)) {
    // The call already failed due to a timeout.
    VLOG_WITH_PREFIX(1) << "Got response to call id " << resp.call_id()
                        << " after client already timed out";
    return Status::OK();
  }

  call->SetResponse(std::move(resp));

  return Status::OK();
}

void Connection::CallSent(OutboundCallPtr call) {
  DCHECK(reactor_->IsCurrentThread());

  awaiting_response_.emplace(call->call_id(), !call->IsFinished() ? call : nullptr);
}

std::string Connection::ToString() const {
  // This may be called from other threads, so we cannot
  // include anything in the output about the current state,
  // which might concurrently change from another thread.
  static const char* format = "Connection ($0) $1 $2 => $3";
  const void* self = this;
  if (direction_ == Direction::SERVER) {
    return Format(format, self, "server", remote(), local());
  } else {
    return Format(format, self, "client", local(), remote());
  }
}

Status Connection::DumpPB(const DumpRunningRpcsRequestPB& req,
                          RpcConnectionPB* resp) {
  DCHECK(reactor_->IsCurrentThread());
  resp->set_remote_ip(yb::ToString(remote()));
  resp->set_state(context_->State());

  const uint64_t processed_call_count =
      direction_ == Direction::CLIENT ? responded_call_count_.load(std::memory_order_acquire)
                                      : context_->ProcessedCallCount();
  if (processed_call_count > 0) {
    resp->set_processed_call_count(processed_call_count);
  }

  context_->DumpPB(req, resp);

  if (direction_ == Direction::CLIENT) {
    auto call_in_flight = resp->add_calls_in_flight();
    for (auto& entry : awaiting_response_) {
      if (entry.second && entry.second->DumpPB(req, call_in_flight)) {
        call_in_flight = resp->add_calls_in_flight();
      }
    }
    resp->mutable_calls_in_flight()->DeleteSubrange(resp->calls_in_flight_size() - 1, 1);
    stream_->DumpPB(req, resp);
  } else if (direction_ != Direction::SERVER) {
    LOG(FATAL) << "Invalid direction: " << to_underlying(direction_);
  }

  return Status::OK();
}

void Connection::QueueOutboundDataBatch(const OutboundDataBatch& batch) {
  DCHECK(reactor_->IsCurrentThread());

  for (const auto& call : batch) {
    DoQueueOutboundData(call, /* batch */ true);
  }

  OutboundQueued();
}

void Connection::QueueOutboundData(OutboundDataPtr outbound_data) {
  if (reactor_->IsCurrentThread()) {
    DoQueueOutboundData(std::move(outbound_data), /* batch */ false);
    return;
  }

  bool was_empty;
  {
    std::unique_lock<simple_spinlock> lock(outbound_data_queue_lock_);
    if (!shutdown_status_.ok()) {
      auto task = MakeFunctorReactorTaskWithAbort(
          std::bind(&OutboundData::Transferred, outbound_data, _2, /* conn */ nullptr),
          SOURCE_LOCATION());
      lock.unlock();
      auto scheduled = reactor_->ScheduleReactorTask(task, true /* schedule_even_closing */);
      LOG_IF_WITH_PREFIX(DFATAL, !scheduled) << "Failed to schedule OutboundData::Transferred";
      return;
    }
    was_empty = outbound_data_to_process_.empty();
    outbound_data_to_process_.push_back(std::move(outbound_data));
    if (was_empty && !process_response_queue_task_) {
      process_response_queue_task_ =
          MakeFunctorReactorTask(std::bind(&Connection::ProcessResponseQueue, this),
                                 shared_from_this(), SOURCE_LOCATION());
    }
  }

  if (was_empty) {
    // TODO: what happens if the reactor is shutting down? Currently Abort is ignored.
    auto scheduled = reactor_->ScheduleReactorTask(process_response_queue_task_);
    LOG_IF_WITH_PREFIX(WARNING, !scheduled)
        << "Failed to schedule Connection::ProcessResponseQueue";
  }
}

void Connection::ProcessResponseQueue() {
  DCHECK(reactor_->IsCurrentThread());

  {
    std::lock_guard<simple_spinlock> lock(outbound_data_queue_lock_);
    outbound_data_to_process_.swap(outbound_data_being_processed_);
  }

  if (!outbound_data_being_processed_.empty()) {
    for (auto &call : outbound_data_being_processed_) {
      DoQueueOutboundData(std::move(call), /* batch */ true);
    }
    outbound_data_being_processed_.clear();
    OutboundQueued();
  }
}

Status Connection::Start(ev::loop_ref* loop) {
  DCHECK(reactor_->IsCurrentThread());

  context_->SetEventLoop(loop);

  RETURN_NOT_OK(stream_->Start(direction_ == Direction::CLIENT, loop, this));

  timer_.Init(*loop);
  timer_.SetCallback<Connection, &Connection::HandleTimeout>(this); // NOLINT

  if (!stream_->IsConnected()) {
    timer_.Start(FLAGS_rpc_connection_timeout_ms * 1ms);
  }

  auto self = shared_from_this();
  context_->AssignConnection(self);

  return Status::OK();
}

void Connection::Connected() {
  context_->Connected(shared_from_this());
}

StreamReadBuffer& Connection::ReadBuffer() {
  return context_->ReadBuffer();
}

const Endpoint& Connection::remote() const {
  return stream_->Remote();
}

const Protocol* Connection::protocol() const {
  return stream_->GetProtocol();
}

const Endpoint& Connection::local() const {
  return stream_->Local();
}

void Connection::Close() {
  stream_->Close();
}

void Connection::UpdateLastActivity() {
  last_activity_time_ = reactor_->cur_time();
  VLOG_WITH_PREFIX(4) << "Updated last_activity_time_=" << AsString(last_activity_time_);
}

void Connection::UpdateLastRead() {
  context_->UpdateLastRead(shared_from_this());
}

void Connection::UpdateLastWrite() {
  context_->UpdateLastWrite(shared_from_this());
}

void Connection::Transferred(const OutboundDataPtr& data, const Status& status) {
  data->Transferred(status, this);
}

void Connection::Destroy(const Status& status) {
  reactor_->DestroyConnection(this, status);
}

std::string Connection::LogPrefix() const {
  return ToString() + ": ";
}

}  // namespace rpc
}  // namespace yb
