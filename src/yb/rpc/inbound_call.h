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
#ifndef YB_RPC_INBOUND_CALL_H_
#define YB_RPC_INBOUND_CALL_H_

#include <string>
#include <vector>

#include <glog/logging.h>

#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/ref_counted.h"

#include "yb/rpc/rpc_fwd.h"
#include "yb/rpc/call_data.h"
#include "yb/rpc/growable_buffer.h"
#include "yb/rpc/rpc_call.h"
#include "yb/rpc/remote_method.h"
#include "yb/rpc/rpc_header.pb.h"
#include "yb/rpc/thread_pool.h"

#include "yb/yql/cql/ql/ql_session.h"

#include "yb/util/faststring.h"
#include "yb/util/lockfree.h"
#include "yb/util/memory/memory.h"
#include "yb/util/monotime.h"
#include "yb/util/ref_cnt_buffer.h"
#include "yb/util/slice.h"
#include "yb/util/status.h"

namespace google {
namespace protobuf {
class Message;
}  // namespace protobuf
}  // namespace google

namespace yb {

class Histogram;
class Trace;

namespace rpc {

class DumpRunningRpcsRequestPB;
class RpcCallInProgressPB;
class RpcCallDetailsPB;
class CQLCallDetailsPB;

struct InboundCallTiming {
  MonoTime time_received;   // Time the call was first accepted.
  MonoTime time_handled;    // Time the call handler was kicked off.
  MonoTime time_completed;  // Time the call handler completed.
};

class InboundCallHandler {
 public:
  virtual void Handle(InboundCallPtr call) = 0;

  virtual void Failure(const InboundCallPtr& call, const Status& status) = 0;

  virtual bool CallQueued() = 0;

  virtual void CallDequeued() = 0;

 protected:
  ~InboundCallHandler() = default;
};

// Inbound call on server
class InboundCall : public RpcCall, public MPSCQueueEntry<InboundCall> {
 public:
  typedef std::function<void(InboundCall*)> CallProcessedListener;

  InboundCall(ConnectionPtr conn, RpcMetrics* rpc_metrics,
              CallProcessedListener call_processed_listener);
  virtual ~InboundCall();

  // Return the serialized request parameter protobuf.
  const Slice &serialized_request() const {
    return serialized_request_;
  }

  virtual const Endpoint& remote_address() const;
  virtual const Endpoint& local_address() const;

  ConnectionPtr connection() const;
  ConnectionContext& connection_context() const;

  Trace* trace();

  // When this InboundCall was received (instantiated).
  // Should only be called once on a given instance.
  // Not thread-safe. Should only be called by the current "owner" thread.
  void RecordCallReceived();

  // When RPC call Handle() was called on the server side.
  // Updates the Histogram with time elapsed since the call was received,
  // and should only be called once on a given instance.
  // Not thread-safe. Should only be called by the current "owner" thread.
  virtual void RecordHandlingStarted(scoped_refptr<Histogram> incoming_queue_time);

  // When RPC call Handle() completed execution on the server side.
  // Updates the Histogram with time elapsed since the call was started,
  // and should only be called once on a given instance.
  // Not thread-safe. Should only be called by the current "owner" thread.
  void RecordHandlingCompleted(scoped_refptr<Histogram> handler_run_time);

  // Return true if the deadline set by the client has already elapsed.
  // In this case, the server may stop processing the call, since the
  // call response will be ignored anyway.
  bool ClientTimedOut() const;

  // Return an upper bound on the client timeout deadline. This does not
  // account for transmission delays between the client and the server.
  // If the client did not specify a deadline, returns MonoTime::Max().
  virtual CoarseTimePoint GetClientDeadline() const = 0;

  // Returns the time spent in the service queue -- from the time the call was received, until
  // it gets handled.
  MonoDelta GetTimeInQueue() const;

  ThreadPoolTask* BindTask(InboundCallHandler* handler) {
    auto shared_this = shared_from(this);
    if (!handler->CallQueued()) {
      return nullptr;
    }
    tracker_ = handler;
    task_.Bind(handler, shared_this);
    return &task_;
  }

  void ResetCallProcessedListener() {
    call_processed_listener_ = decltype(call_processed_listener_)();
  }

  virtual const std::string& method_name() const = 0;
  virtual const std::string& service_name() const = 0;
  virtual void RespondFailure(ErrorStatusPB::RpcErrorCodePB error_code, const Status& status) = 0;

  // Do appropriate actions when call is timed out.
  //
  // message contains human readable information on why call timed out.
  //
  // Returns true if actions were applied, false if call was already processed.
  bool RespondTimedOutIfPending(const char* message);

  bool TryStartProcessing() {
    bool expected = false;
    if (!processing_started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return false;
    }
    if (tracker_) {
      tracker_->CallDequeued();
    }
    return true;
  }

  std::string LogPrefix() const override;

  template <class T, class ...Args>
  static std::shared_ptr<T> Create(Args&&... args) {
    auto result = std::make_shared<T>(std::forward<Args>(args)...);
    result->RecordCallReceived();
    return result;
  }

  size_t DynamicMemoryUsage() const override;

  const CallData& request_data() const { return request_data_; }

 protected:
  void NotifyTransferred(const Status& status, Connection* conn) override;

  virtual void Clear();

  // Log a WARNING message if the RPC response was slow enough that the
  // client likely timed out. This is based on the client-provided timeout
  // value.
  // Also can be configured to log _all_ RPC traces for help debugging.
  virtual void LogTrace() const = 0;

  void QueueResponse(bool is_success);

  // The serialized bytes of the request param protobuf. Set by ParseFrom().
  // This references memory held by 'request_data_'.
  Slice serialized_request_;

  // Data source of this call.
  CallData request_data_;

  // The trace buffer.
  scoped_refptr<Trace> trace_;

  // Timing information related to this RPC call.
  InboundCallTiming timing_;

  std::atomic<bool> processing_started_{false};

  std::atomic<bool> responded_{false};

 private:
  // The connection on which this inbound call arrived. Can be null for LocalYBInboundCall.
  ConnectionPtr conn_ = nullptr;
  RpcMetrics* rpc_metrics_;
  std::function<void(InboundCall*)> call_processed_listener_;

  class InboundCallTask : public ThreadPoolTask {
   public:
    void Bind(InboundCallHandler* handler, InboundCallPtr call) {
      handler_ = handler;
      call_ = std::move(call);
    }

    void Run() override;

    void Done(const Status& status) override;

    virtual ~InboundCallTask() = default;

   private:
    InboundCallHandler* handler_;
    InboundCallPtr call_;
  };

  InboundCallTask task_;
  InboundCallHandler* tracker_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(InboundCall);
};

}  // namespace rpc
}  // namespace yb

#endif  // YB_RPC_INBOUND_CALL_H_
