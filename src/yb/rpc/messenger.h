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
#ifndef YB_RPC_MESSENGER_H_
#define YB_RPC_MESSENGER_H_

#include <stdint.h>

#include <atomic>
#include <memory>
#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <gtest/gtest_prod.h>

#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/ref_counted.h"

#include "yb/rpc/rpc_fwd.h"
#include "yb/rpc/io_thread_pool.h"
#include "yb/rpc/proxy.h"
#include "yb/rpc/reactor.h"
#include "yb/rpc/response_callback.h"
#include "yb/rpc/scheduler.h"

#include "yb/util/concurrent_value.h"
#include "yb/util/debug-util.h"
#include "yb/util/locks.h"
#include "yb/util/metrics.h"
#include "yb/util/monotime.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/status.h"

namespace yb {

class MemTracker;
class Socket;

namespace rpc {

template <class ContextType>
class ConnectionContextFactoryImpl;

typedef std::unordered_map<const Protocol*, StreamFactoryPtr> StreamFactories;

// Used to construct a Messenger.
class MessengerBuilder {
 public:
  friend class Messenger;

  explicit MessengerBuilder(std::string name);

  // Set the length of time we will keep a TCP connection will alive with no traffic.
  MessengerBuilder &set_connection_keepalive_time(CoarseMonoClock::Duration keepalive);

  // Set the number of reactor threads that will be used for sending and receiving.
  MessengerBuilder &set_num_reactors(int num_reactors);

  // Set the granularity with which connections are checked for keepalive.
  MessengerBuilder &set_coarse_timer_granularity(CoarseMonoClock::Duration granularity);

  // Set metric entity for use by RPC systems.
  MessengerBuilder &set_metric_entity(const scoped_refptr<MetricEntity>& metric_entity);

  // Uses the given connection type to handle the incoming connections.
  MessengerBuilder &UseConnectionContextFactory(const ConnectionContextFactoryPtr& factory) {
    connection_context_factory_ = factory;
    return *this;
  }

  MessengerBuilder &UseDefaultConnectionContextFactory(
      const std::shared_ptr<MemTracker>& parent_mem_tracker = nullptr);

  MessengerBuilder &AddStreamFactory(const Protocol* protocol, StreamFactoryPtr factory);

  MessengerBuilder &SetListenProtocol(const Protocol* protocol) {
    listen_protocol_ = protocol;
    return *this;
  }

  template <class ContextType>
  MessengerBuilder &CreateConnectionContextFactory(
      size_t memory_limit, const std::shared_ptr<MemTracker>& parent_mem_tracker = nullptr) {
    if (parent_mem_tracker) {
      last_used_parent_mem_tracker_ = parent_mem_tracker;
    }
    connection_context_factory_ =
        std::make_shared<ConnectionContextFactoryImpl<ContextType>>(
            memory_limit, parent_mem_tracker);
    return *this;
  }

  Result<std::unique_ptr<Messenger>> Build();

  CoarseMonoClock::Duration connection_keepalive_time() const {
    return connection_keepalive_time_;
  }

  CoarseMonoClock::Duration coarse_timer_granularity() const {
    return coarse_timer_granularity_;
  }

  const ConnectionContextFactoryPtr& connection_context_factory() const {
    return connection_context_factory_;
  }

  MessengerBuilder& set_thread_pool_options(size_t queue_limit, size_t workers_limit) {
    queue_limit_ = queue_limit;
    workers_limit_ = workers_limit;
    return *this;
  }

  MessengerBuilder& set_num_connections_to_server(int value) {
    num_connections_to_server_ = value;
    return *this;
  }

  int num_connections_to_server() const {
    return num_connections_to_server_;
  }

  const std::shared_ptr<MemTracker>& last_used_parent_mem_tracker() const {
    return last_used_parent_mem_tracker_;
  }

 private:
  const std::string name_;
  CoarseMonoClock::Duration connection_keepalive_time_;
  int num_reactors_ = 4;
  CoarseMonoClock::Duration coarse_timer_granularity_ = std::chrono::milliseconds(100);
  scoped_refptr<MetricEntity> metric_entity_;
  ConnectionContextFactoryPtr connection_context_factory_;
  StreamFactories stream_factories_;
  const Protocol* listen_protocol_;
  size_t queue_limit_;
  size_t workers_limit_;
  int num_connections_to_server_;
  std::shared_ptr<MemTracker> last_used_parent_mem_tracker_;
};

// A Messenger is a container for the reactor threads which run event loops for the RPC services.
// If the process is a server, a Messenger will also have an Acceptor.  In this case, calls received
// over the connection are enqueued into the messenger's service_queue for processing by a
// ServicePool.
//
// Users do not typically interact with the Messenger directly except to create one as a singleton,
// and then make calls using Proxy objects.
//
// See rpc-test.cc and rpc-bench.cc for example usages.
class Messenger : public ProxyContext {
 public:
  friend class MessengerBuilder;
  friend class Proxy;
  friend class Reactor;
  typedef std::unordered_map<std::string, scoped_refptr<RpcService>> RpcServicesMap;

  ~Messenger();

  // Stop all communication and prevent further use. Should be called explicitly by messenger owner.
  void Shutdown();

  // Setup messenger to listen connections on given address.
  CHECKED_STATUS ListenAddress(
      ConnectionContextFactoryPtr factory, const Endpoint& accept_endpoint,
      Endpoint* bound_endpoint = nullptr);

  // Stop accepting connections.
  void ShutdownAcceptor();

  // Start accepting connections.
  CHECKED_STATUS StartAcceptor();

  // Register a new RpcService to handle inbound requests.
  CHECKED_STATUS RegisterService(const std::string& service_name,
                         const scoped_refptr<RpcService>& service);

  // Unregister currently-registered RpcService.
  CHECKED_STATUS UnregisterService(const std::string& service_name);

  void UnregisterAllServices();

  void ShutdownThreadPools();

  // Queue a call for transmission. This will pick the appropriate reactor, and enqueue a task on
  // that reactor to assign and send the call.
  void QueueOutboundCall(OutboundCallPtr call) override;

  // Enqueue a call for processing on the server.
  void QueueInboundCall(InboundCallPtr call) override;

  // Invoke the RpcService to handle a call directly.
  void Handle(InboundCallPtr call) override;

  const Protocol* DefaultProtocol() override { return listen_protocol_; }

  rpc::ThreadPool& CallbackThreadPool(ServicePriority priority) override {
    return ThreadPool(priority);
  }

  CHECKED_STATUS QueueEventOnAllReactors(
      ServerEventListPtr server_event, const SourceLocation& source_location);

  // Dump the current RPCs into the given protobuf.
  CHECKED_STATUS DumpRunningRpcs(const DumpRunningRpcsRequestPB& req,
                                 DumpRunningRpcsResponsePB* resp);

  void RemoveScheduledTask(ScheduledTaskId task_id);

  // This method will run 'func' with an ABORT status argument. It's not guaranteed that the task
  // will cancel because TimerHandler could run before this method.
  void AbortOnReactor(ScheduledTaskId task_id);

  // Run 'func' on a reactor thread after 'when' time elapses.
  //
  // The status argument conveys whether 'func' was run correctly (i.e. after the elapsed time) or
  // not.
  MUST_USE_RESULT ScheduledTaskId ScheduleOnReactor(
      StatusFunctor func, MonoDelta when,
      const SourceLocation& source_location,
      rpc::Messenger* msgr);

  std::string name() const {
    return name_;
  }

  scoped_refptr<MetricEntity> metric_entity() const override { return metric_entity_; }

  scoped_refptr<RpcService> rpc_service(const std::string& service_name) const;

  size_t max_concurrent_requests() const;

  const IpAddress& outbound_address_v4() const { return outbound_address_v4_; }
  const IpAddress& outbound_address_v6() const { return outbound_address_v6_; }

  void BreakConnectivityWith(const IpAddress& address);
  void BreakConnectivityTo(const IpAddress& address);
  void BreakConnectivityFrom(const IpAddress& address);
  void RestoreConnectivityWith(const IpAddress& address);
  void RestoreConnectivityTo(const IpAddress& address);
  void RestoreConnectivityFrom(const IpAddress& address);

  Scheduler& scheduler() {
    return scheduler_;
  }

  IoService& io_service() override {
    return io_thread_pool_.io_service();
  }

  DnsResolver& resolver() override {
    return *resolver_;
  }

  rpc::ThreadPool& ThreadPool(ServicePriority priority = ServicePriority::kNormal);

  RpcMetrics& rpc_metrics() override {
    return *rpc_metrics_;
  }

  const std::shared_ptr<MemTracker>& parent_mem_tracker() override;

  int num_connections_to_server() const override {
    return num_connections_to_server_;
  }

  // Use specified IP address as base address for outbound connections from messenger.
  void TEST_SetOutboundIpBase(const IpAddress& value) {
    test_outbound_ip_base_ = value;
    has_outbound_ip_base_.store(true, std::memory_order_release);
  }

  bool TEST_ShouldArtificiallyRejectIncomingCallsFrom(const IpAddress &remote);

  CHECKED_STATUS TEST_GetReactorMetrics(size_t reactor_idx, ReactorMetrics* metrics);

 private:
  friend class DelayedTask;

  explicit Messenger(const MessengerBuilder &bld);

  Reactor* RemoteToReactor(const Endpoint& remote, uint32_t idx = 0);
  CHECKED_STATUS Init();
  void UpdateServicesCache(std::lock_guard<percpu_rwlock>* guard);

  void BreakConnectivity(const IpAddress& address, bool incoming, bool outgoing);
  void RestoreConnectivity(const IpAddress& address, bool incoming, bool outgoing);

  // Take ownership of the socket via Socket::Release
  void RegisterInboundSocket(
      const ConnectionContextFactoryPtr& factory, Socket *new_socket, const Endpoint& remote);

  bool TEST_ShouldArtificiallyRejectOutgoingCallsTo(const IpAddress &remote);

  const std::string name_;

  ConnectionContextFactoryPtr connection_context_factory_;

  const StreamFactories stream_factories_;

  const Protocol* const listen_protocol_;

  // Protects closing_, acceptor_pools_, rpc_services_.
  mutable percpu_rwlock lock_;

  bool closing_ = false;

  // RPC services that handle inbound requests.
  RpcServicesMap rpc_services_;
  mutable ConcurrentValue<RpcServicesMap> rpc_services_cache_;

  std::vector<std::unique_ptr<Reactor>> reactors_;

  const scoped_refptr<MetricEntity> metric_entity_;
  const scoped_refptr<Histogram> outgoing_queue_time_;

  // Acceptor which is listening on behalf of this messenger.
  std::unique_ptr<Acceptor> acceptor_;
  IpAddress outbound_address_v4_;
  IpAddress outbound_address_v6_;

  // Id that will be assigned to the next task that is scheduled on the reactor.
  std::atomic<ScheduledTaskId> next_task_id_ = {1};
  std::atomic<uint64_t> num_connections_accepted_ = {0};

  std::mutex mutex_scheduled_tasks_;

  std::unordered_map<ScheduledTaskId, std::shared_ptr<DelayedTask>> scheduled_tasks_;

  // Flag that we have at least on address with artificially broken connectivity.
  std::atomic<bool> has_broken_connectivity_ = {false};

  // Set of addresses with artificially broken connectivity.
  std::unordered_set<IpAddress, IpAddressHash> broken_connectivity_from_;
  std::unordered_set<IpAddress, IpAddressHash> broken_connectivity_to_;

  IoThreadPool io_thread_pool_;
  Scheduler scheduler_;

  // Thread pools that are used by services running in this messenger.
  std::unique_ptr<rpc::ThreadPool> normal_thread_pool_;

  std::mutex mutex_high_priority_thread_pool_;

  // This could be used for high-priority services such as Consensus.
  AtomicUniquePtr<rpc::ThreadPool> high_priority_thread_pool_;

  std::unique_ptr<DnsResolver> resolver_;

  std::unique_ptr<RpcMetrics> rpc_metrics_;

  // Use this IP address as base address for outbound connections from messenger.
  IpAddress test_outbound_ip_base_;
  std::atomic<bool> has_outbound_ip_base_{false};

  // Number of outbound connections to create per each destination server address.
  int num_connections_to_server_;

#ifndef NDEBUG
  // This is so we can log where exactly a Messenger was instantiated to better diagnose a CHECK
  // failure in the destructor (ENG-2838). This can be removed when that is fixed.
  StackTrace creation_stack_trace_;
#endif
  DISALLOW_COPY_AND_ASSIGN(Messenger);
};

}  // namespace rpc
}  // namespace yb

#endif  // YB_RPC_MESSENGER_H_
