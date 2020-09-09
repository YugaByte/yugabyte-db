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

#include "yb/rpc/messenger.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <list>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/rpc/acceptor.h"
#include "yb/rpc/connection.h"
#include "yb/rpc/constants.h"
#include "yb/rpc/proxy.h"
#include "yb/rpc/rpc_header.pb.h"
#include "yb/rpc/rpc_metrics.h"
#include "yb/rpc/rpc_service.h"
#include "yb/rpc/rpc_util.h"
#include "yb/rpc/tcp_stream.h"
#include "yb/rpc/yb_rpc.h"

#include "yb/util/errno.h"
#include "yb/util/flag_tags.h"
#include "yb/util/logging.h"
#include "yb/util/metrics.h"
#include "yb/util/monotime.h"
#include "yb/util/net/dns_resolver.h"
#include "yb/util/net/socket.h"
#include "yb/util/scope_exit.h"
#include "yb/util/size_literals.h"
#include "yb/util/status.h"
#include "yb/util/threadpool.h"
#include "yb/util/thread_restrictions.h"
#include "yb/util/trace.h"

using namespace std::literals;
using namespace std::placeholders;
using namespace yb::size_literals;

using std::string;
using std::shared_ptr;
using strings::Substitute;

DECLARE_int32(num_connections_to_server);
DEFINE_int32(rpc_default_keepalive_time_ms, 65000,
             "If an RPC connection from a client is idle for this amount of time, the server "
             "will disconnect the client. Setting flag to 0 disables this clean up.");
TAG_FLAG(rpc_default_keepalive_time_ms, advanced);
DEFINE_uint64(io_thread_pool_size, 4, "Size of allocated IO Thread Pool.");

DEFINE_int64(outbound_rpc_memory_limit, 0, "Outbound RPC memory limit");

DEFINE_int32(rpc_queue_limit, 10000, "Queue limit for rpc server");
DEFINE_int32(rpc_workers_limit, 1024, "Workers limit for rpc server");

DEFINE_int32(socket_receive_buffer_size, 0, "Socket receive buffer size, 0 to use default");

namespace yb {
namespace rpc {

class Messenger;
class ServerBuilder;

// ------------------------------------------------------------------------------------------------
// MessengerBuilder
// ------------------------------------------------------------------------------------------------

MessengerBuilder::MessengerBuilder(std::string name)
    : name_(std::move(name)),
      connection_keepalive_time_(FLAGS_rpc_default_keepalive_time_ms * 1ms),
      coarse_timer_granularity_(100ms),
      listen_protocol_(TcpStream::StaticProtocol()),
      queue_limit_(FLAGS_rpc_queue_limit),
      workers_limit_(FLAGS_rpc_workers_limit),
      num_connections_to_server_(GetAtomicFlag(&FLAGS_num_connections_to_server)) {
  AddStreamFactory(TcpStream::StaticProtocol(), TcpStream::Factory());
}

MessengerBuilder& MessengerBuilder::set_connection_keepalive_time(
    CoarseMonoClock::Duration keepalive) {
  connection_keepalive_time_ = keepalive;
  return *this;
}

MessengerBuilder& MessengerBuilder::set_num_reactors(int num_reactors) {
  num_reactors_ = num_reactors;
  return *this;
}

MessengerBuilder& MessengerBuilder::set_coarse_timer_granularity(
    CoarseMonoClock::Duration granularity) {
  coarse_timer_granularity_ = granularity;
  return *this;
}

MessengerBuilder &MessengerBuilder::set_metric_entity(
    const scoped_refptr<MetricEntity>& metric_entity) {
  metric_entity_ = metric_entity;
  return *this;
}

Result<std::unique_ptr<Messenger>> MessengerBuilder::Build() {
  if (!connection_context_factory_) {
    UseDefaultConnectionContextFactory();
  }
  std::unique_ptr<Messenger> messenger(new Messenger(*this));
  RETURN_NOT_OK(messenger->Init());

  return messenger;
}

MessengerBuilder &MessengerBuilder::AddStreamFactory(
    const Protocol* protocol, StreamFactoryPtr factory) {
  auto p = stream_factories_.emplace(protocol, std::move(factory));
  LOG_IF(DFATAL, !p.second) << "Duplicate stream factory: " << protocol->ToString();
  return *this;
}

MessengerBuilder &MessengerBuilder::UseDefaultConnectionContextFactory(
    const std::shared_ptr<MemTracker>& parent_mem_tracker) {
  if (parent_mem_tracker) {
    last_used_parent_mem_tracker_ = parent_mem_tracker;
  }
  connection_context_factory_ = rpc::CreateConnectionContextFactory<YBOutboundConnectionContext>(
      FLAGS_outbound_rpc_memory_limit, parent_mem_tracker);
  return *this;
}

// ------------------------------------------------------------------------------------------------
// Messenger
// ------------------------------------------------------------------------------------------------

void Messenger::Shutdown() {
  ShutdownThreadPools();
  ShutdownAcceptor();
  UnregisterAllServices();

  // Since we're shutting down, it's OK to block.
  ThreadRestrictions::ScopedAllowWait allow_wait;

  std::vector<Reactor*> reactors;
  std::unique_ptr<Acceptor> acceptor;
  {
    std::lock_guard<percpu_rwlock> guard(lock_);
    if (closing_) {
      return;
    }
    VLOG(1) << "shutting down messenger " << name_;
    closing_ = true;

    DCHECK(rpc_services_.empty()) << "Unregister RPC services before shutting down Messenger";
    rpc_services_.clear();

    acceptor.swap(acceptor_);

    for (const auto& reactor : reactors_) {
      reactors.push_back(reactor.get());
    }
  }

  if (acceptor) {
    acceptor->Shutdown();
  }

  for (auto* reactor : reactors) {
    reactor->Shutdown();
  }

  scheduler_.Shutdown();
  io_thread_pool_.Shutdown();

  for (auto* reactor : reactors) {
    reactor->Join();
  }

  io_thread_pool_.Join();

  {
    std::lock_guard<std::mutex> guard(mutex_scheduled_tasks_);
    LOG_IF(DFATAL, !scheduled_tasks_.empty())
        << "Scheduled tasks is not empty after messenger shutdown: "
        << yb::ToString(scheduled_tasks_);
  }
}

Status Messenger::ListenAddress(
    ConnectionContextFactoryPtr factory, const Endpoint& accept_endpoint,
    Endpoint* bound_endpoint) {
  Acceptor* acceptor;
  {
    std::lock_guard<percpu_rwlock> guard(lock_);
    if (!acceptor_) {
      acceptor_.reset(new Acceptor(
          metric_entity_, std::bind(&Messenger::RegisterInboundSocket, this, factory, _1, _2)));
    }
    auto accept_host = accept_endpoint.address();
    auto& outbound_address = accept_host.is_v6() ? outbound_address_v6_
                                                 : outbound_address_v4_;
    if (outbound_address.is_unspecified() && !accept_host.is_unspecified()) {
      outbound_address = accept_host;
    }
    acceptor = acceptor_.get();
  }
  return acceptor->Listen(accept_endpoint, bound_endpoint);
}

Status Messenger::StartAcceptor() {
  std::lock_guard<percpu_rwlock> guard(lock_);
  if (acceptor_) {
    return acceptor_->Start();
  } else {
    return STATUS(IllegalState, "Trying to start acceptor w/o active addresses");
  }
}

void Messenger::BreakConnectivityWith(const IpAddress& address) {
  BreakConnectivity(address, /* incoming */ true, /* outgoing */ true);
}
void Messenger::BreakConnectivityTo(const IpAddress& address) {
  BreakConnectivity(address, /* incoming */ false, /* outgoing */ true);
}

void Messenger::BreakConnectivityFrom(const IpAddress& address) {
  BreakConnectivity(address, /* incoming */ true, /* outgoing */ false);
}

void Messenger::BreakConnectivity(const IpAddress& address, bool incoming, bool outgoing) {
  LOG(INFO) << "TEST: Break " << (incoming ? "incoming" : "") << "/" << (outgoing ? "outgoing" : "")
            << " connectivity with: " << address;

  boost::optional<CountDownLatch> latch;
  {
    std::lock_guard<percpu_rwlock> guard(lock_);
    if (broken_connectivity_from_.empty() || broken_connectivity_to_.empty()) {
      has_broken_connectivity_.store(true, std::memory_order_release);
    }
    bool inserted_from = false;
    if (incoming) {
      inserted_from = broken_connectivity_from_.insert(address).second;
    }
    bool inserted_to = false;
    if (outgoing) {
      inserted_to = broken_connectivity_to_.insert(address).second;
    }
    if (inserted_from || inserted_to) {
      latch.emplace(reactors_.size());
      for (const auto& reactor : reactors_) {
        auto scheduled = reactor->ScheduleReactorTask(MakeFunctorReactorTask(
            [&latch, address, incoming, outgoing](Reactor* reactor) {
              if (incoming) {
                reactor->DropIncomingWithRemoteAddress(address);
              }
              if (outgoing) {
                reactor->DropOutgoingWithRemoteAddress(address);
              }
              latch->CountDown();
            },
            SOURCE_LOCATION()));
        if (!scheduled) {
          LOG(INFO) << "Failed to schedule drop connection with: " << address.to_string();
          latch->CountDown();
        }
      }
    }
  }

  if (latch) {
    latch->Wait();
  }
}

void Messenger::RestoreConnectivityWith(const IpAddress& address) {
  RestoreConnectivity(address, /* incoming */ true, /* outgoing */ true);
}
void Messenger::RestoreConnectivityTo(const IpAddress& address) {
  RestoreConnectivity(address, /* incoming */ false, /* outgoing */ true);
}

void Messenger::RestoreConnectivityFrom(const IpAddress& address) {
  RestoreConnectivity(address, /* incoming */ true, /* outgoing */ false);
}

void Messenger::RestoreConnectivity(const IpAddress& address, bool incoming, bool outgoing) {
  LOG(INFO) << "TEST: Restore " << (incoming ? "incoming" : "") << "/"
            << (outgoing ? "outgoing" : "") << " connectivity with: " << address;

  std::lock_guard<percpu_rwlock> guard(lock_);
  if (incoming) {
    broken_connectivity_from_.erase(address);
  }
  if (outgoing) {
    broken_connectivity_to_.erase(address);
  }
  if (broken_connectivity_from_.empty() && broken_connectivity_to_.empty()) {
    has_broken_connectivity_.store(false, std::memory_order_release);
  }
}

bool Messenger::TEST_ShouldArtificiallyRejectIncomingCallsFrom(const IpAddress &remote) {
  if (has_broken_connectivity_.load(std::memory_order_acquire)) {
    shared_lock<rw_spinlock> guard(lock_.get_lock());
    return broken_connectivity_from_.count(remote) != 0;
  }
  return false;
}

bool Messenger::TEST_ShouldArtificiallyRejectOutgoingCallsTo(const IpAddress &remote) {
  if (has_broken_connectivity_.load(std::memory_order_acquire)) {
    shared_lock<rw_spinlock> guard(lock_.get_lock());
    return broken_connectivity_to_.count(remote) != 0;
  }
  return false;
}

Status Messenger::TEST_GetReactorMetrics(size_t reactor_idx, ReactorMetrics* metrics) {
  if (reactor_idx >= reactors_.size()) {
    return STATUS_FORMAT(
        InvalidArgument, "Invalid reactor index $0, should be >=0 and <$1", reactor_idx,
        reactors_.size());
  }
  return reactors_[reactor_idx]->GetMetrics(metrics);
}

void Messenger::ShutdownAcceptor() {
  std::unique_ptr<Acceptor> acceptor;
  {
    std::lock_guard<percpu_rwlock> guard(lock_);
    acceptor.swap(acceptor_);
  }
  if (acceptor) {
    acceptor->Shutdown();
  }
}

rpc::ThreadPool& Messenger::ThreadPool(ServicePriority priority) {
  switch (priority) {
    case ServicePriority::kNormal:
      return *normal_thread_pool_;
    case ServicePriority::kHigh:
      auto high_priority_thread_pool = high_priority_thread_pool_.get();
      if (high_priority_thread_pool) {
        return *high_priority_thread_pool;
      }
      std::lock_guard<std::mutex> lock(mutex_high_priority_thread_pool_);
      high_priority_thread_pool = high_priority_thread_pool_.get();
      if (high_priority_thread_pool) {
        return *high_priority_thread_pool;
      }
      const ThreadPoolOptions& options = normal_thread_pool_->options();
      high_priority_thread_pool_.reset(new rpc::ThreadPool(
          name_ + "-high-pri", options.queue_limit, options.max_workers));
      return *high_priority_thread_pool_.get();
  }
  FATAL_INVALID_ENUM_VALUE(ServicePriority, priority);
}

// Register a new RpcService to handle inbound requests.
Status Messenger::RegisterService(const string& service_name,
                                  const scoped_refptr<RpcService>& service) {
  DCHECK(service);
  std::lock_guard<percpu_rwlock> guard(lock_);
  if (InsertIfNotPresent(&rpc_services_, service_name, service)) {
    UpdateServicesCache(&guard);
    return Status::OK();
  } else {
    return STATUS_SUBSTITUTE(AlreadyPresent, "Service $0 is already present", service_name);
  }
}

void Messenger::ShutdownThreadPools() {
  normal_thread_pool_->Shutdown();
  auto high_priority_thread_pool = high_priority_thread_pool_.get();
  if (high_priority_thread_pool) {
    high_priority_thread_pool->Shutdown();
  }
}

void Messenger::UnregisterAllServices() {
  decltype(rpc_services_) rpc_services_copy; // Drain rpc services here,
                                             // to avoid deleting them in locked state.
  {
    std::lock_guard<percpu_rwlock> guard(lock_);
    rpc_services_.swap(rpc_services_copy);
    UpdateServicesCache(&guard);
  }

  for (const auto& p : rpc_services_copy) {
    p.second->StartShutdown();
  }
  for (const auto& p : rpc_services_copy) {
    p.second->CompleteShutdown();
  }
  rpc_services_copy.clear();
}

// Unregister an RpcService.
Status Messenger::UnregisterService(const string& service_name) {
  scoped_refptr<RpcService> service;
  {
    std::lock_guard<percpu_rwlock> guard(lock_);
    auto it = rpc_services_.find(service_name);
    if (it == rpc_services_.end()) {
      return STATUS(ServiceUnavailable, Substitute("service $0 not registered on $1",
                   service_name, name_));
    }
    service = it->second;
    rpc_services_.erase(it);
    UpdateServicesCache(&guard);
  }
  service->StartShutdown();
  service->CompleteShutdown();
  return Status::OK();
}

class NotifyDisconnectedReactorTask : public ReactorTask {
 public:
  NotifyDisconnectedReactorTask(OutboundCallPtr call, const SourceLocation& source_location)
      : ReactorTask(source_location), call_(std::move(call)) {}

  void Run(Reactor* reactor) override  {
    call_->Transferred(STATUS_FORMAT(
        NetworkError, "TEST: Connectivity is broken with $0",
        call_->conn_id().remote().address()), nullptr);
  }
 private:
  void DoAbort(const Status &abort_status) override {
    call_->Transferred(abort_status, nullptr);
  }

  OutboundCallPtr call_;
};

void Messenger::QueueOutboundCall(OutboundCallPtr call) {
  const auto& remote = call->conn_id().remote();
  Reactor *reactor = RemoteToReactor(remote, call->conn_id().idx());

  if (TEST_ShouldArtificiallyRejectOutgoingCallsTo(remote.address())) {
    VLOG(1) << "TEST: Rejected connection to " << remote;
    auto scheduled = reactor->ScheduleReactorTask(std::make_shared<NotifyDisconnectedReactorTask>(
        call, SOURCE_LOCATION()));
    if (!scheduled) {
      call->Transferred(STATUS(Aborted, "Reactor is closing"), nullptr /* conn */);
    }
    return;
  }

  reactor->QueueOutboundCall(std::move(call));
}

void Messenger::QueueInboundCall(InboundCallPtr call) {
  auto service = rpc_service(call->service_name());
  if (PREDICT_FALSE(!service)) {
    Status s = STATUS_FORMAT(ServiceUnavailable,
                             "Service $0 not registered on $1",
                             call->service_name(),
                             name_);
    LOG(WARNING) << s;
    call->RespondFailure(ErrorStatusPB::ERROR_NO_SUCH_SERVICE, s);
    return;
  }

  // The RpcService will respond to the client on success or failure.
  service->QueueInboundCall(std::move(call));
}

void Messenger::Handle(InboundCallPtr call) {
  auto service = rpc_service(call->service_name());
  if (PREDICT_FALSE(!service)) {
    Status s = STATUS_FORMAT(ServiceUnavailable,
                             "Service $0 not registered on $1",
                             call->service_name(),
                             name_);
    LOG(WARNING) << s;
    call->RespondFailure(ErrorStatusPB::ERROR_NO_SUCH_SERVICE, s);
    return;
  }

  service->Handle(std::move(call));
}

const std::shared_ptr<MemTracker>& Messenger::parent_mem_tracker() {
  return connection_context_factory_->buffer_tracker();
}

void Messenger::RegisterInboundSocket(
    const ConnectionContextFactoryPtr& factory, Socket *new_socket, const Endpoint& remote) {
  if (TEST_ShouldArtificiallyRejectIncomingCallsFrom(remote.address())) {
    auto status = new_socket->Close();
    VLOG(1) << "TEST: Rejected connection from " << remote
            << ", close status: " << status.ToString();
    return;
  }

  if (FLAGS_socket_receive_buffer_size) {
    WARN_NOT_OK(new_socket->SetReceiveBufferSize(FLAGS_socket_receive_buffer_size),
                "Set receive buffer size failed: ");
  }

  auto receive_buffer_size = new_socket->GetReceiveBufferSize();
  if (!receive_buffer_size.ok()) {
    LOG(WARNING) << "Register inbound socket failed: " << receive_buffer_size.status();
    return;
  }

  int idx = num_connections_accepted_.fetch_add(1) % num_connections_to_server_;
  Reactor *reactor = RemoteToReactor(remote, idx);
  reactor->RegisterInboundSocket(
      new_socket, remote, factory->Create(*receive_buffer_size), factory->buffer_tracker());
}

Messenger::Messenger(const MessengerBuilder &bld)
    : name_(bld.name_),
      connection_context_factory_(bld.connection_context_factory_),
      stream_factories_(bld.stream_factories_),
      listen_protocol_(bld.listen_protocol_),
      metric_entity_(bld.metric_entity_),
      io_thread_pool_(name_, FLAGS_io_thread_pool_size),
      scheduler_(&io_thread_pool_.io_service()),
      normal_thread_pool_(new rpc::ThreadPool(name_, bld.queue_limit_, bld.workers_limit_)),
      resolver_(new DnsResolver(&io_thread_pool_.io_service())),
      rpc_metrics_(new RpcMetrics(bld.metric_entity_)),
      num_connections_to_server_(bld.num_connections_to_server_) {
#ifndef NDEBUG
  creation_stack_trace_.Collect(/* skip_frames */ 1);
#endif
  VLOG(1) << "Messenger constructor for " << this << " called at:\n" << GetStackTrace();
  for (int i = 0; i < bld.num_reactors_; i++) {
    reactors_.emplace_back(std::make_unique<Reactor>(this, i, bld));
  }
  // Make sure skip buffer is allocated before we hit memory limit and try to use it.
  GetGlobalSkipBuffer();
}

Messenger::~Messenger() {
  std::lock_guard<percpu_rwlock> guard(lock_);
  // This logging and the corresponding logging in the constructor is here to track down the
  // occasional CHECK(closing_) failure below in some tests (ENG-2838).
  VLOG(1) << "Messenger destructor for " << this << " called at:\n" << GetStackTrace();
#ifndef NDEBUG
  if (!closing_) {
    LOG(ERROR) << "Messenger created here:\n" << creation_stack_trace_.Symbolize()
               << "Messenger destructor for " << this << " called at:\n" << GetStackTrace();
  }
#endif
  CHECK(closing_) << "Should have already shut down";
  reactors_.clear();
}

size_t Messenger::max_concurrent_requests() const {
  return num_connections_to_server_;
}

Reactor* Messenger::RemoteToReactor(const Endpoint& remote, uint32_t idx) {
  uint32_t hashCode = hash_value(remote);
  int reactor_idx = (hashCode + idx) % reactors_.size();
  // This is just a static partitioning; where each connection
  // to a remote is assigned to a particular reactor. We could
  // get a lot fancier with assigning Sockaddrs to Reactors,
  // but this should be good enough.
  return reactors_[reactor_idx].get();
}

Status Messenger::Init() {
  Status status;
  for (const auto& r : reactors_) {
    RETURN_NOT_OK(r->Init());
  }

  return Status::OK();
}

Status Messenger::DumpRunningRpcs(const DumpRunningRpcsRequestPB& req,
                                  DumpRunningRpcsResponsePB* resp) {
  shared_lock<rw_spinlock> guard(lock_.get_lock());
  for (const auto& reactor : reactors_) {
    RETURN_NOT_OK(reactor->DumpRunningRpcs(req, resp));
  }
  return Status::OK();
}

Status Messenger::QueueEventOnAllReactors(
    ServerEventListPtr server_event, const SourceLocation& source_location) {
  shared_lock<rw_spinlock> guard(lock_.get_lock());
  for (const auto& reactor : reactors_) {
    reactor->QueueEventOnAllConnections(server_event, source_location);
  }
  return Status::OK();
}

void Messenger::RemoveScheduledTask(ScheduledTaskId id) {
  CHECK_GT(id, 0);
  std::lock_guard<std::mutex> guard(mutex_scheduled_tasks_);
  scheduled_tasks_.erase(id);
}

void Messenger::AbortOnReactor(ScheduledTaskId task_id) {
  DCHECK(!reactors_.empty());
  CHECK_GT(task_id, 0);

  std::shared_ptr<DelayedTask> task;
  {
    std::lock_guard<std::mutex> guard(mutex_scheduled_tasks_);
    auto iter = scheduled_tasks_.find(task_id);
    if (iter != scheduled_tasks_.end()) {
      task = iter->second;
      scheduled_tasks_.erase(iter);
    }
  }
  if (task) {
    task->AbortTask(STATUS(Aborted, "Task aborted by messenger"));
  }
}

ScheduledTaskId Messenger::ScheduleOnReactor(
    StatusFunctor func, MonoDelta when, const SourceLocation& source_location, Messenger* msgr) {
  DCHECK(!reactors_.empty());

  // If we're already running on a reactor thread, reuse it.
  Reactor* chosen = nullptr;
  for (const auto& r : reactors_) {
    if (r->IsCurrentThread()) {
      chosen = r.get();
    }
  }
  if (chosen == nullptr) {
    // Not running on a reactor thread, pick one at random.
    chosen = reactors_[rand() % reactors_.size()].get();
  }

  ScheduledTaskId task_id = 0;
  if (msgr != nullptr) {
    task_id = next_task_id_.fetch_add(1);
  }
  auto task = std::make_shared<DelayedTask>(
      std::move(func), when, task_id, source_location, msgr);
  if (msgr != nullptr) {
    std::lock_guard<std::mutex> guard(mutex_scheduled_tasks_);
    scheduled_tasks_.emplace(task_id, task);
  }

  if (chosen->ScheduleReactorTask(task)) {
    return task_id;
  }

  {
    std::lock_guard<std::mutex> guard(mutex_scheduled_tasks_);
    scheduled_tasks_.erase(task_id);
  }

  return kInvalidTaskId;
}

void Messenger::UpdateServicesCache(std::lock_guard<percpu_rwlock>* guard) {
  DCHECK_ONLY_NOTNULL(guard);

  rpc_services_cache_.Set(rpc_services_);
}

scoped_refptr<RpcService> Messenger::rpc_service(const string& service_name) const {
  auto cache = rpc_services_cache_.get();
  auto it = cache->find(service_name);
  if (it != cache->end()) {
    // Since our cache is a cache of whole rpc_services_ map, we could check only it.
    return it->second;
  }
  return scoped_refptr<RpcService>();
}

} // namespace rpc
} // namespace yb
