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

#include "yb/rpc/reactor.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <functional>
#include <mutex>
#include <string>

#include <ev++.h>

#include <glog/logging.h>

#include "yb/gutil/ref_counted.h"
#include "yb/gutil/stringprintf.h"
#include "yb/rpc/connection.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/rpc_controller.h"
#include "yb/rpc/rpc_introspection.pb.h"
#include "yb/rpc/yb_rpc.h"

#include "yb/util/countdown_latch.h"
#include "yb/util/errno.h"
#include "yb/util/flag_tags.h"
#include "yb/util/memory/memory.h"
#include "yb/util/monotime.h"
#include "yb/util/scope_exit.h"
#include "yb/util/status.h"
#include "yb/util/thread.h"
#include "yb/util/threadpool.h"
#include "yb/util/thread_restrictions.h"
#include "yb/util/trace.h"
#include "yb/util/net/socket.h"

using namespace std::literals;

DECLARE_string(local_ip_for_outbound_sockets);
DECLARE_int32(num_connections_to_server);
DECLARE_int32(socket_receive_buffer_size);

namespace yb {
namespace rpc {

namespace {

static const char* kShutdownMessage = "Shutdown connection";

const Status& AbortedError() {
  static Status result = STATUS(Aborted, kShutdownMessage, "" /* msg2 */, Errno(ESHUTDOWN));
  return result;
}

const Status& ServiceUnavailableError() {
  static Status result = STATUS(
      ServiceUnavailable, kShutdownMessage, "" /* msg2 */, Errno(ESHUTDOWN));
  return result;
}

// Callback for libev fatal errors (eg running out of file descriptors).
// Unfortunately libev doesn't plumb these back through to the caller, but
// instead just expects the callback to abort.
//
// This implementation is slightly preferable to the built-in one since
// it uses a FATAL log message instead of printing to stderr, which might
// not end up anywhere useful in a daemonized context.
void LibevSysErr(const char* msg) throw() {
  PLOG(FATAL) << "LibEV fatal error: " << msg;
}

void DoInitLibEv() {
  ev::set_syserr_cb(LibevSysErr);
}

bool HasReactorStartedClosing(ReactorState state) {
  return state == ReactorState::kClosing || state == ReactorState::kClosed;
}

} // anonymous namespace

// ------------------------------------------------------------------------------------------------
// Reactor class members
// ------------------------------------------------------------------------------------------------

Reactor::Reactor(Messenger* messenger,
                 int index,
                 const MessengerBuilder &bld)
    : messenger_(messenger),
      name_(StringPrintf("%s_R%03d", messenger->name().c_str(), index)),
      log_prefix_(name_ + ": "),
      loop_(kDefaultLibEvFlags),
      cur_time_(CoarseMonoClock::Now()),
      last_unused_tcp_scan_(cur_time_),
      connection_keepalive_time_(bld.connection_keepalive_time()),
      coarse_timer_granularity_(bld.coarse_timer_granularity()),
      num_connections_to_server_(bld.num_connections_to_server()) {
  static std::once_flag libev_once;
  std::call_once(libev_once, DoInitLibEv);

  VLOG_WITH_PREFIX(1) << "Create reactor with keep alive_time: "
                      << yb::ToString(connection_keepalive_time_)
                      << ", coarse timer granularity: " << yb::ToString(coarse_timer_granularity_);

  process_outbound_queue_task_ =
      MakeFunctorReactorTask(std::bind(&Reactor::ProcessOutboundQueue, this), SOURCE_LOCATION());
}

Reactor::~Reactor() {
  LOG_IF_WITH_PREFIX(DFATAL, !pending_tasks_.empty())
      << "Not empty pending tasks when destroyed reactor: " << yb::ToString(pending_tasks_);
}

Status Reactor::Init() {
  DCHECK(thread_.get() == nullptr) << "Already started";
  DVLOG_WITH_PREFIX(6) << "Called Reactor::Init()";
  // Register to get async notifications in our epoll loop.
  async_.set(loop_);
  async_.set<Reactor, &Reactor::AsyncHandler>(this);
  async_.start();

  // Register the timer watcher.
  // The timer is used for closing old TCP connections and applying
  // backpressure.
  timer_.set(loop_);
  timer_.set<Reactor, &Reactor::TimerHandler>(this);
  timer_.start(ToSeconds(coarse_timer_granularity_),
               ToSeconds(coarse_timer_granularity_));

  // Create Reactor thread.
  const std::string group_name = messenger_->name() + "_reactor";
  return yb::Thread::Create(group_name, group_name, &Reactor::RunThread, this, &thread_);
}

void Reactor::Shutdown() {
  ReactorState old_state = ReactorState::kRunning;
  do {
    if (state_.compare_exchange_weak(old_state,
                                     ReactorState::kClosing,
                                     std::memory_order_acq_rel)) {
      VLOG_WITH_PREFIX(1) << "shutting down Reactor thread.";
      WakeThread();
    }
  } while (!HasReactorStartedClosing(old_state));

  // Another thread already switched the state to closing before us.
}

void Reactor::ShutdownConnection(const ConnectionPtr& conn) {
  DCHECK(IsCurrentThread());

  VLOG_WITH_PREFIX(1) << "shutting down " << conn->ToString();
  conn->Shutdown(ServiceUnavailableError());
  if (!conn->context().Idle()) {
    VLOG_WITH_PREFIX(1) << "connection is not idle: " << conn->ToString();
    std::weak_ptr<Connection> weak_conn(conn);
    conn->context().ListenIdle([this, weak_conn]() {
      DCHECK(IsCurrentThreadOrStartedClosing());
      auto conn = weak_conn.lock();
      if (conn) {
        VLOG_WITH_PREFIX(1) << "connection became idle " << conn->ToString();
        // The access to waiting_conns_ is safe here, because this code can only be called on the
        // reactor thread or when holding final_abort_mutex_ during shutdown.
        waiting_conns_.erase(conn);
      }
    });
    waiting_conns_.insert(conn);
  } else {
    VLOG_WITH_PREFIX(1) << "connection is idle: " << conn->ToString();
  }
}

void Reactor::ShutdownInternal() {
  DCHECK(IsCurrentThread());

  stopping_ = true;
  stop_start_time_ = CoarseMonoClock::Now();

  // Tear down any outbound TCP connections.
  VLOG_WITH_PREFIX(1) << "tearing down outbound TCP connections...";
  decltype(client_conns_) client_conns = std::move(client_conns_);
  for (auto& pair : client_conns) {
    ShutdownConnection(pair.second);
  }
  client_conns.clear();

  // Tear down any inbound TCP connections.
  VLOG_WITH_PREFIX(1) << "tearing down inbound TCP connections...";
  for (const ConnectionPtr& conn : server_conns_) {
    ShutdownConnection(conn);
  }
  server_conns_.clear();

  // Abort any scheduled tasks.
  //
  // These won't be found in the Reactor's list of pending tasks
  // because they've been "run" (that is, they've been scheduled).
  VLOG_WITH_PREFIX(1) << "aborting scheduled tasks";
  Status aborted = AbortedError();
  for (const auto& task : scheduled_tasks_) {
    task->Abort(aborted);
  }
  scheduled_tasks_.clear();

  // async_handler_tasks_ are the tasks added by ScheduleReactorTask.
  VLOG_WITH_PREFIX(1) << "aborting async handler tasks";
  for (const auto& task : async_handler_tasks_) {
    task->Abort(aborted);
  }

  VLOG_WITH_PREFIX(1) << "aborting outbound calls";
  CHECK(processing_outbound_queue_.empty()) << yb::ToString(processing_outbound_queue_);
  {
    std::lock_guard<simple_spinlock> lock(outbound_queue_lock_);
    outbound_queue_stopped_ = true;
    outbound_queue_.swap(processing_outbound_queue_);
  }
  for (auto& call : processing_outbound_queue_) {
    call->Transferred(aborted, nullptr);
  }
  processing_outbound_queue_.clear();
}

Status Reactor::GetMetrics(ReactorMetrics *metrics) {
  return RunOnReactorThread([metrics](Reactor* reactor) {
    metrics->num_client_connections_ = reactor->client_conns_.size();
    metrics->num_server_connections_ = reactor->server_conns_.size();
    return Status::OK();
  }, SOURCE_LOCATION());
}

void Reactor::Join() {
  auto join_result = ThreadJoiner(thread_.get()).give_up_after(30s).Join();
  if (join_result.ok()) {
    return;
  }
  if (join_result.IsInvalidArgument()) {
    LOG_WITH_PREFIX(WARNING) << join_result;
    return;
  }
  LOG_WITH_PREFIX(DFATAL) << "Failed to join Reactor " << thread_->ToString() << ": "
                          << join_result;
  // Fallback to endless join in release mode.
  thread_->Join();
}

void Reactor::QueueEventOnAllConnections(
    ServerEventListPtr server_event, const SourceLocation& source_location) {
  ScheduleReactorFunctor([server_event = std::move(server_event)](Reactor* reactor) {
    for (const ConnectionPtr& conn : reactor->server_conns_) {
      conn->QueueOutboundData(server_event);
    }
  }, source_location);
}

Status Reactor::DumpRunningRpcs(const DumpRunningRpcsRequestPB& req,
                                DumpRunningRpcsResponsePB* resp) {
  return RunOnReactorThread([&req, resp](Reactor* reactor) -> Status {
    for (const ConnectionPtr& conn : reactor->server_conns_) {
      RETURN_NOT_OK(conn->DumpPB(req, resp->add_inbound_connections()));
    }
    for (const auto& entry : reactor->client_conns_) {
      Connection* conn = entry.second.get();
      RETURN_NOT_OK(conn->DumpPB(req, resp->add_outbound_connections()));
    }
    return Status::OK();
  }, SOURCE_LOCATION());
}

void Reactor::WakeThread() {
  async_.send();
}

void Reactor::CheckReadyToStop() {
  DCHECK(IsCurrentThread());

  VLOG_WITH_PREFIX(4) << "Check ready to stop: " << thread_->ToString() << ", "
          << "waiting connections: " << yb::ToString(waiting_conns_);

  if (VLOG_IS_ON(4)) {
    for (const auto& conn : waiting_conns_) {
      VLOG_WITH_PREFIX(4) << "Connection: " << conn->ToString() << ", idle=" << conn->Idle()
                          << ", why: " << conn->ReasonNotIdle();
    }
  }

  if (waiting_conns_.empty()) {
    VLOG_WITH_PREFIX(4) << "Reactor ready to stop, breaking loop: " << this;

    VLOG_WITH_PREFIX(2) << "Marking reactor as closed: " << thread_.get()->ToString();
    ReactorTasks final_tasks;
    {
      std::lock_guard<simple_spinlock> lock(pending_tasks_mtx_);
      state_.store(ReactorState::kClosed, std::memory_order_release);
      final_tasks.swap(pending_tasks_);
    }
    VLOG_WITH_PREFIX(2) << "Running final pending task aborts: " << thread_.get()->ToString();;
    for (auto task : final_tasks) {
      task->Abort(ServiceUnavailableError());
    }
    VLOG_WITH_PREFIX(2) << "Breaking reactor loop: " << thread_.get()->ToString();;
    loop_.break_loop(); // break the epoll loop and terminate the thread
  }
}

// Handle async events.  These events are sent to the reactor by other threads that want to bring
// something to our attention, like the fact that we're shutting down, or the fact that there is a
// new outbound Transfer ready to send.
void Reactor::AsyncHandler(ev::async &watcher, int revents) {
  DCHECK(IsCurrentThread());

  auto se = ScopeExit([this] {
    async_handler_tasks_.clear();
  });

  if (PREDICT_FALSE(DrainTaskQueueAndCheckIfClosing())) {
    ShutdownInternal();
    CheckReadyToStop();
    return;
  }

  for (const auto &task : async_handler_tasks_) {
    task->Run(this);
  }
}

void Reactor::RegisterConnection(const ConnectionPtr& conn) {
  DCHECK(IsCurrentThread());

  Status s = conn->Start(&loop_);
  if (s.ok()) {
    server_conns_.push_back(conn);
  } else {
    LOG_WITH_PREFIX(WARNING) << "Failed to start connection: " << conn->ToString() << ": " << s;
  }
}

ConnectionPtr Reactor::AssignOutboundCall(const OutboundCallPtr& call) {
  DCHECK(IsCurrentThread());
  ConnectionPtr conn;

  // TODO: Move call deadline timeout computation into OutboundCall constructor.
  const MonoDelta &timeout = call->controller()->timeout();
  MonoTime deadline;
  if (!timeout.Initialized()) {
    LOG_WITH_PREFIX(WARNING) << "Client call " << call->remote_method().ToString()
                 << " has no timeout set for connection id: "
                 << call->conn_id().ToString();
    deadline = MonoTime::Max();
  } else {
    deadline = MonoTime::Now();
    deadline.AddDelta(timeout);
  }

  Status s = FindOrStartConnection(call->conn_id(), call->hostname(), deadline, &conn);
  if (PREDICT_FALSE(!s.ok())) {
    call->SetFailed(s);
    return ConnectionPtr();
  }

  conn->QueueOutboundCall(call);
  return conn;
}

//
// Handles timer events.  The periodic timer:
//
// 1. updates Reactor::cur_time_
// 2. every tcp_conn_timeo_ seconds, close down connections older than
//    tcp_conn_timeo_ seconds.
//
void Reactor::TimerHandler(ev::timer &watcher, int revents) {
  DCHECK(IsCurrentThread());

  if (EV_ERROR & revents) {
    LOG_WITH_PREFIX(WARNING) << "Reactor got an error in the timer handler.";
    return;
  }

  if (stopping_) {
    CheckReadyToStop();
    return;
  }

  auto now = CoarseMonoClock::Now();
  VLOG_WITH_PREFIX(4) << "timer tick at " << ToSeconds(now.time_since_epoch());
  cur_time_ = now;

  ScanIdleConnections();
}

void Reactor::ScanIdleConnections() {
  DCHECK(IsCurrentThread());
  if (connection_keepalive_time_ == CoarseMonoClock::Duration::zero()) {
    VLOG_WITH_PREFIX(3) << "Skipping Idle connections check since connection_keepalive_time_ = 0";
    return;
  }

  // enforce TCP connection timeouts
  auto c = server_conns_.begin();
  auto c_end = server_conns_.end();
  uint64_t timed_out = 0;
  for (; c != c_end; ) {
    const ConnectionPtr& conn = *c;
    if (!conn->Idle()) {
      VLOG_WITH_PREFIX(3) << "Connection " << conn->ToString() << " not idle";
      ++c; // TODO: clean up this loop
      continue;
    }

    auto last_activity_time = conn->last_activity_time();
    auto connection_delta = cur_time_ - last_activity_time;
    if (connection_delta > connection_keepalive_time_) {
      conn->Shutdown(STATUS_FORMAT(
          NetworkError, "Connection timed out after $0", ToSeconds(connection_delta)));
      LOG_WITH_PREFIX(INFO)
          << "DEBUG: Closing idle connection: " << conn->ToString()
          << " - it has been idle for " << ToSeconds(connection_delta) << "s";
      VLOG(1) << "(delta: " << ToSeconds(connection_delta)
          << ", current time: " << ToSeconds(cur_time_.time_since_epoch())
          << ", last activity time: " << ToSeconds(last_activity_time.time_since_epoch()) << ")";
      server_conns_.erase(c++);
      ++timed_out;
    } else {
      ++c;
    }
  }

  // TODO: above only times out on the server side.
  // Clients may want to set their keepalive timeout as well.

  VLOG_IF_WITH_PREFIX(1, timed_out > 0) << "timed out " << timed_out << " TCP connections.";
}

bool Reactor::IsCurrentThread() const {
  return thread_.get() == yb::Thread::current_thread();
}

bool Reactor::IsCurrentThreadOrStartedClosing() const {
  return thread_.get() == yb::Thread::current_thread() ||
         HasReactorStartedClosing(state_.load(std::memory_order_acquire));
}

void Reactor::RunThread() {
  ThreadRestrictions::SetWaitAllowed(false);
  ThreadRestrictions::SetIOAllowed(false);
  DVLOG_WITH_PREFIX(6) << "Calling Reactor::RunThread()...";
  loop_.run(/* flags */ 0);
  VLOG_WITH_PREFIX(1) << "thread exiting.";
}

namespace {

Result<Socket> CreateClientSocket(const Endpoint& remote) {
  int flags = Socket::FLAG_NONBLOCKING;
  if (remote.address().is_v6()) {
    flags |= Socket::FLAG_IPV6;
  }
  Socket socket;
  Status status = socket.Init(flags);
  if (status.ok()) {
    status = socket.SetNoDelay(true);
  }
  LOG_IF(WARNING, !status.ok()) << "failed to create an "
      "outbound connection because a new socket could not "
      "be created: " << status.ToString();
  if (!status.ok())
    return status;
  return std::move(socket);
}

template <class... Args>
Result<std::unique_ptr<Stream>> CreateStream(
    const StreamFactories& factories, const Protocol* protocol, const StreamCreateData& data) {
  auto it = factories.find(protocol);
  if (it == factories.end()) {
    return STATUS_FORMAT(NotFound, "Unknown protocol: $0", protocol);
  }
  return it->second->Create(data);
}

} // namespace

Status Reactor::FindOrStartConnection(const ConnectionId &conn_id,
                                      const std::string& hostname,
                                      const MonoTime &deadline,
                                      ConnectionPtr* conn) {
  DCHECK(IsCurrentThread());
  auto c = client_conns_.find(conn_id);
  if (c != client_conns_.end()) {
    *conn = (*c).second;
    return Status::OK();
  }

  if (HasReactorStartedClosing(state_.load(std::memory_order_acquire))) {
    return ServiceUnavailableError();
  }

  // No connection to this remote. Need to create one.
  VLOG_WITH_PREFIX(2) << "FindOrStartConnection: creating new connection for "
                      << conn_id.ToString();

  // Create a new socket and start connecting to the remote.
  auto sock = VERIFY_RESULT(CreateClientSocket(conn_id.remote()));
  if (messenger_->has_outbound_ip_base_.load(std::memory_order_acquire) &&
      !messenger_->test_outbound_ip_base_.is_unspecified()) {
    auto address_bytes(messenger_->test_outbound_ip_base_.to_v4().to_bytes());
    // Use different addresses for public/private endpoints.
    // Private addresses are even, and public are odd.
    // So if base address is "private" and destination address is "public" we will modify
    // originating address to be "public" also.
    address_bytes[3] |= conn_id.remote().address().to_v4().to_bytes()[3] & 1;
    boost::asio::ip::address_v4 outbound_address(address_bytes);
    auto status = sock.Bind(Endpoint(outbound_address, 0));
    LOG_IF_WITH_PREFIX(WARNING, !status.ok()) << "Bind " << outbound_address << " failed: "
                                              << status;
  } else if (FLAGS_local_ip_for_outbound_sockets.empty()) {
    auto outbound_address = conn_id.remote().address().is_v6()
        ? messenger_->outbound_address_v6()
        : messenger_->outbound_address_v4();
    if (!outbound_address.is_unspecified()) {
      auto status = sock.Bind(Endpoint(outbound_address, 0));
      LOG_IF_WITH_PREFIX(WARNING, !status.ok()) << "Bind " << outbound_address << " failed: "
                                                << status;
    }
  }

  if (FLAGS_socket_receive_buffer_size) {
    WARN_NOT_OK(sock.SetReceiveBufferSize(FLAGS_socket_receive_buffer_size),
                "Set receive buffer size failed: ");
  }

  auto receive_buffer_size = VERIFY_RESULT(sock.GetReceiveBufferSize());

  auto context = messenger_->connection_context_factory_->Create(receive_buffer_size);
  auto stream = VERIFY_RESULT(CreateStream(
      messenger_->stream_factories_, conn_id.protocol(),
      {conn_id.remote(), hostname, &sock,
       messenger_->connection_context_factory_->buffer_tracker()}));

  // Register the new connection in our map.
  auto connection = std::make_shared<Connection>(
      this,
      std::move(stream),
      ConnectionDirection::CLIENT,
      &messenger()->rpc_metrics(),
      std::move(context));

  RETURN_NOT_OK(connection->Start(&loop_));

  // Insert into the client connection map to avoid duplicate connection requests.
  CHECK(client_conns_.emplace(conn_id, connection).second);

  conn->swap(connection);
  return Status::OK();
}

namespace {

void ShutdownIfRemoteAddressIs(const ConnectionPtr& conn, const IpAddress& address) {
  Endpoint peer = conn->remote();

  if (peer.address() != address) {
    return;
  }

  conn->Close();
  LOG(INFO) << "Dropped connection: " << conn->ToString();
}

} // namespace

void Reactor::DropWithRemoteAddress(const IpAddress& address) {
  DropIncomingWithRemoteAddress(address);
  DropOutgoingWithRemoteAddress(address);
}

void Reactor::DropIncomingWithRemoteAddress(const IpAddress& address) {
  DCHECK(IsCurrentThread());

  VLOG_WITH_PREFIX(1) << "Dropping Incoming connections from " << address;
  for (auto& conn : server_conns_) {
    ShutdownIfRemoteAddressIs(conn, address);
  }
}

void Reactor::DropOutgoingWithRemoteAddress(const IpAddress& address) {
  DCHECK(IsCurrentThread());
  VLOG_WITH_PREFIX(1) << "Dropping Outgoing connections to " << address;
  for (auto& pair : client_conns_) {
    ShutdownIfRemoteAddressIs(pair.second, address);
  }
}

void Reactor::DestroyConnection(Connection *conn, const Status &conn_status) {
  DCHECK(IsCurrentThread());

  VLOG_WITH_PREFIX(3) << "DestroyConnection(" << conn->ToString() << ", " << conn_status.ToString()
                      << ")";

  ConnectionPtr retained_conn = conn->shared_from_this();
  conn->Shutdown(conn_status);

  // Unlink connection from lists.
  if (conn->direction() == ConnectionDirection::CLIENT) {
    bool erased = false;
    for (int idx = 0; idx < num_connections_to_server_; idx++) {
      auto it = client_conns_.find(ConnectionId(conn->remote(), idx, conn->protocol()));
      if (it != client_conns_.end() && it->second.get() == conn) {
        client_conns_.erase(it);
        erased = true;
      }
    }
    if (!erased) {
      LOG_WITH_PREFIX(WARNING) << "Looking for " << conn->ToString();
      for (auto &p : client_conns_) {
        LOG_WITH_PREFIX(WARNING) << "  Client connection: " << p.first.ToString() << ", "
                     << p.second->ToString();
      }
    }
    CHECK(erased) << "Couldn't find connection for any index to " << conn->ToString();
  } else if (conn->direction() == ConnectionDirection::SERVER) {
    auto it = server_conns_.begin();
    while (it != server_conns_.end()) {
      if ((*it).get() == conn) {
        server_conns_.erase(it);
        break;
      }
      ++it;
    }
  }

  ShutdownConnection(retained_conn);
}

void Reactor::ProcessOutboundQueue() {
  CHECK(processing_outbound_queue_.empty()) << yb::ToString(processing_outbound_queue_);
  {
    std::lock_guard<simple_spinlock> lock(outbound_queue_lock_);
    outbound_queue_.swap(processing_outbound_queue_);
  }
  if (processing_outbound_queue_.empty()) {
    return;
  }

  processing_connections_.reserve(processing_outbound_queue_.size());
  for (auto& call : processing_outbound_queue_) {
    auto conn = AssignOutboundCall(call);
    processing_connections_.push_back(std::move(conn));
  }
  processing_outbound_queue_.clear();

  std::sort(processing_connections_.begin(), processing_connections_.end());
  auto new_end = std::unique(processing_connections_.begin(), processing_connections_.end());
  processing_connections_.erase(new_end, processing_connections_.end());
  for (auto& conn : processing_connections_) {
    if (conn) {
      conn->OutboundQueued();
    }
  }
  processing_connections_.clear();
}

void Reactor::QueueOutboundCall(OutboundCallPtr call) {
  DVLOG_WITH_PREFIX(3) << "Queueing outbound call "
                       << call->ToString() << " to remote " << call->conn_id().remote();

  bool was_empty = false;
  bool closing = false;
  {
    std::lock_guard<simple_spinlock> lock(outbound_queue_lock_);
    if (!outbound_queue_stopped_) {
      was_empty = outbound_queue_.empty();
      outbound_queue_.push_back(call);
    } else {
      closing = true;
    }
  }
  if (closing) {
    call->Transferred(AbortedError(), nullptr /* conn */);
    return;
  }
  if (was_empty) {
    auto scheduled = ScheduleReactorTask(process_outbound_queue_task_);
    LOG_IF_WITH_PREFIX(WARNING, !scheduled) << "Failed to schedule process outbound queue task";
  }
  TRACE_TO(call->trace(), "Scheduled.");
}

// ------------------------------------------------------------------------------------------------
// ReactorTask class members
// ------------------------------------------------------------------------------------------------

ReactorTask::ReactorTask(const SourceLocation& source_location)
    : source_location_(source_location) {
}

ReactorTask::~ReactorTask() {
}

void ReactorTask::Abort(const Status& abort_status) {
  if (!abort_called_.exchange(true, std::memory_order_acq_rel)) {
    DoAbort(abort_status);
  }
}

std::string ReactorTask::ToString() const {
  return Format("{ source: $0 }", source_location_);
}

// ------------------------------------------------------------------------------------------------
// DelayedTask class members
// ------------------------------------------------------------------------------------------------

DelayedTask::DelayedTask(StatusFunctor func, MonoDelta when, int64_t id,
                         const SourceLocation& source_location, Messenger* messenger)
    : ReactorTask(source_location),
      func_(std::move(func)),
      when_(when),
      id_(id),
      messenger_(messenger) {
}

void DelayedTask::Run(Reactor* reactor) {
  DCHECK(reactor_ == nullptr) << "Task has already been scheduled";
  DCHECK(reactor->IsCurrentThread());

  // Acquire lock to prevent task from being aborted in the middle of scheduling, in case abort
  // will be requested in the middle of scheduling - task will be aborted right after return
  // from this method.
  std::lock_guard<LockType> l(lock_);
  if (done_) {
    // Task has been aborted.
    return;
  }

  // Schedule the task to run later.
  reactor_ = reactor;
  timer_.set(reactor->loop_);

  // timer_ is owned by this task and will be stopped through AbortTask/Abort before this task
  // is removed from list of scheduled tasks, so it is safe for timer_ to remember pointer to task.
  timer_.set<DelayedTask, &DelayedTask::TimerHandler>(this);

  timer_.start(when_.ToSeconds(), // after
               0);                // repeat
  reactor_->scheduled_tasks_.insert(shared_from(this));
}

MarkAsDoneResult DelayedTask::MarkAsDone() {
  std::lock_guard<LockType> l(lock_);
  if (done_) {
    return MarkAsDoneResult::kAlreadyDone;
  }
  done_ = true;

  // ENG-2879: we need to check if reactor_ is nullptr, because that would mean that the task has
  // not even started.  AbortTask uses the return value of this function to check if it needs to
  // stop the timer, and that is only possible / necessary if Run has been called and reactor_ is
  // not nullptr.
  return reactor_ == nullptr ? MarkAsDoneResult::kNotScheduled
                             : MarkAsDoneResult::kSuccess;
}

std::string DelayedTask::ToString() const {
  return Format("{ id: $0 source: $1 }", id_, source_location_);
}

void DelayedTask::AbortTask(const Status& abort_status) {
  auto mark_as_done_result = MarkAsDone();
  if (mark_as_done_result == MarkAsDoneResult::kSuccess) {
    // Stop the libev timer. We don't need to do this in the kNotScheduled case, because the timer
    // has not started in that case.
    if (reactor_->IsCurrentThread()) {
      timer_.stop();
    } else {
      // Must call timer_.stop() on the reactor thread. Keep a refcount to prevent this DelayedTask
      // from being deleted. If the reactor thread has already been shut down, this will be a no-op.
      reactor_->ScheduleReactorFunctor([this, holder = shared_from(this)](Reactor* reactor) {
        timer_.stop();
      }, SOURCE_LOCATION());
    }
  }
  if (mark_as_done_result != MarkAsDoneResult::kAlreadyDone) {
    // We need to call the callback whenever we successfully switch the done_ flag to true, whether
    // or not the task has been scheduled.
    func_(abort_status);
  }
}

void DelayedTask::DoAbort(const Status& abort_status) {
  if (messenger_ != nullptr) {
    messenger_->RemoveScheduledTask(id_);
  }

  AbortTask(abort_status);
}

void DelayedTask::TimerHandler(ev::timer& watcher, int revents) {
  DCHECK(reactor_->IsCurrentThread());

  auto mark_as_done_result = MarkAsDone();
  if (mark_as_done_result != MarkAsDoneResult::kSuccess) {
    DCHECK_EQ(MarkAsDoneResult::kAlreadyDone, mark_as_done_result)
        << "Can't get kNotScheduled here, because the timer handler is already being called";
    return;
  }

  // Hold shared_ptr, so this task wouldn't be destroyed upon removal below until func_ is called.
  auto holder = shared_from_this();

  reactor_->scheduled_tasks_.erase(shared_from(this));
  if (messenger_ != nullptr) {
    messenger_->RemoveScheduledTask(id_);
  }

  if (EV_ERROR & revents) {
    std::string msg = "Delayed task got an error in its timer handler";
    LOG(WARNING) << msg;
    func_(STATUS(Aborted, msg));
  } else {
    func_(Status::OK());
  }
}

// ------------------------------------------------------------------------------------------------
// More Reactor class members
// ------------------------------------------------------------------------------------------------

void Reactor::RegisterInboundSocket(
    Socket *socket, const Endpoint& remote, std::unique_ptr<ConnectionContext> connection_context,
    const MemTrackerPtr& mem_tracker) {
  VLOG_WITH_PREFIX(3) << "New inbound connection to " << remote;

  auto stream = CreateStream(
      messenger_->stream_factories_, messenger_->listen_protocol_,
      {remote, std::string(), socket, mem_tracker});
  if (!stream.ok()) {
    LOG_WITH_PREFIX(DFATAL) << "Failed to create stream for " << remote << ": " << stream.status();
    return;
  }
  auto conn = std::make_shared<Connection>(this,
                                           std::move(*stream),
                                           ConnectionDirection::SERVER,
                                           &messenger()->rpc_metrics(),
                                           std::move(connection_context));
  ScheduleReactorFunctor([conn = std::move(conn)](Reactor* reactor) {
    reactor->RegisterConnection(conn);
  }, SOURCE_LOCATION());
}

bool Reactor::ScheduleReactorTask(ReactorTaskPtr task, bool schedule_even_closing) {
  bool was_empty;
  {
    // Even though state_ is atomic, we still need to take the lock to make sure state_
    // and pending_tasks_mtx_ are being modified in a consistent way.
    std::unique_lock<simple_spinlock> pending_lock(pending_tasks_mtx_);
    auto state = state_.load(std::memory_order_acquire);
    bool failure = schedule_even_closing ? state == ReactorState::kClosed
                                         : HasReactorStartedClosing(state);
    if (failure) {
      return false;
    }
    was_empty = pending_tasks_.empty();
    pending_tasks_.emplace_back(std::move(task));
  }
  if (was_empty) {
    WakeThread();
  }

  return true;
}

bool Reactor::DrainTaskQueueAndCheckIfClosing() {
  CHECK(async_handler_tasks_.empty());

  std::lock_guard<simple_spinlock> lock(pending_tasks_mtx_);
  async_handler_tasks_.swap(pending_tasks_);
  return HasReactorStartedClosing(state_.load(std::memory_order_acquire));
}

// Task to call an arbitrary function within the reactor thread.
template<class F>
class RunFunctionTask : public ReactorTask {
 public:
  RunFunctionTask(const F& f, const SourceLocation& source_location)
      : ReactorTask(source_location), function_(f) {}

  void Run(Reactor *reactor) override {
    status_ = function_(reactor);
    latch_.CountDown();
  }

  // Wait until the function has completed, and return the Status returned by the function.
  Status Wait() {
    latch_.Wait();
    return status_;
  }

 private:
  void DoAbort(const Status &status) override {
    status_ = status;
    latch_.CountDown();
  }

  F function_;
  Status status_;
  CountDownLatch latch_{1};
};

template<class F>
Status Reactor::RunOnReactorThread(const F& f, const SourceLocation& source_location) {
  auto task = std::make_shared<RunFunctionTask<F>>(f, source_location);
  if (!ScheduleReactorTask(task)) {
    return ServiceUnavailableError();
  }
  return task->Wait();
}

}  // namespace rpc
}  // namespace yb
