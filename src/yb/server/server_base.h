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
#ifndef YB_SERVER_SERVER_BASE_H
#define YB_SERVER_SERVER_BASE_H

#include <memory>
#include <string>

#include "yb/common/wire_protocol.pb.h"
#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/ref_counted.h"
#include "yb/rpc/service_if.h"
#include "yb/server/server_base_options.h"
#include "yb/server/webserver.h"
#include "yb/util/status.h"

namespace yb {

class Env;
class FsManager;
class MemTracker;
class MetricEntity;
class MetricRegistry;
class NodeInstancePB;
class ScopedGLogMetrics;
class Thread;
class Webserver;

namespace server {

class Clock;
class RpcServer;
class ServerBaseOptions;
class ServerStatusPB;

// Base class that is common to implementing a Redis server, as well as
// a YB tablet server and master.
class RpcServerBase {
 public:
  const RpcServer *rpc_server() const { return rpc_server_.get(); }
  rpc::Messenger* messenger() const { return messenger_.get(); }
  rpc::ProxyCache& proxy_cache() { return *proxy_cache_; }

  // Return the first RPC address that this server has bound to.
  // FATALs if the server is not started.
  Endpoint first_rpc_address() const;

  // Return the instance identifier of this server.
  // This may not be called until after the server is Initted.
  const NodeInstancePB& instance_pb() const;

  const std::shared_ptr<MemTracker>& mem_tracker() const { return mem_tracker_; }

  const scoped_refptr<MetricEntity>& metric_entity() const { return metric_entity_; }

  MetricRegistry* metric_registry() { return metric_registry_.get(); }

  // Returns this server's clock.
  Clock* clock() { return clock_.get(); }

  virtual std::string ToString() const;

  // Return a PB describing the status of the server (version info, bound ports, etc)
  virtual void GetStatusPB(ServerStatusPB* status) const;

  CloudInfoPB MakeCloudInfoPB() const {
    return options_.MakeCloudInfoPB();
  }

  const ServerBaseOptions& options() const {
    return options_;
  }
  // Return the hostname of this server
  const std::string get_hostname() const;

  // Return the current user logged in to this server
  const std::string get_current_user() const;

 protected:
  RpcServerBase(std::string name,
                const ServerBaseOptions& options,
                const std::string& metrics_namespace,
                std::shared_ptr<MemTracker> mem_tracker);
  virtual ~RpcServerBase();

  CHECKED_STATUS Init();
  CHECKED_STATUS RegisterService(
      size_t queue_limit, rpc::ServiceIfPtr rpc_impl,
      rpc::ServicePriority priority = rpc::ServicePriority::kNormal);
  CHECKED_STATUS Start();
  CHECKED_STATUS StartRpcServer();
  void Shutdown();
  void SetConnectionContextFactory(rpc::ConnectionContextFactoryPtr connection_context_factory);
  virtual CHECKED_STATUS SetupMessengerBuilder(rpc::MessengerBuilder* builder);

  const std::string name_;
  std::shared_ptr<MemTracker> mem_tracker_;
  gscoped_ptr<MetricRegistry> metric_registry_;
  scoped_refptr<MetricEntity> metric_entity_;
  gscoped_ptr<RpcServer> rpc_server_;
  std::unique_ptr<rpc::Messenger> messenger_;
  std::unique_ptr<rpc::ProxyCache> proxy_cache_;

  scoped_refptr<Clock> clock_;

  // The instance identifier of this server.
  gscoped_ptr<NodeInstancePB> instance_pb_;

  ServerBaseOptions options_;

  virtual CHECKED_STATUS DumpServerInfo(const std::string& path,
                        const std::string& format) const;

  bool initialized_;
 private:
  CHECKED_STATUS StartMetricsLogging();
  void MetricsLoggingThread();

  scoped_refptr<Thread> metrics_logging_thread_;
  CountDownLatch stop_metrics_logging_latch_;

  gscoped_ptr<ScopedGLogMetrics> glog_metrics_;

  DISALLOW_COPY_AND_ASSIGN(RpcServerBase);
};

YB_STRONGLY_TYPED_BOOL(RpcOnly);

// Base class for tablet server and master.
// Handles starting and stopping the RPC server and web server,
// and provides a common interface for server-type-agnostic functions.
class RpcAndWebServerBase : public RpcServerBase {
 public:
  const Webserver *web_server() const { return web_server_.get(); }

  FsManager* fs_manager() { return fs_manager_.get(); }

  // Return the first HTTP address that this server has bound to.
  // FATALs if the server is not started.
  Endpoint first_http_address() const;

  // Return a PB describing the status of the server (version info, bound ports, etc)
  void GetStatusPB(ServerStatusPB* status) const override;

  // Centralized method to get the Registration information for either the Master or Tserver.
  CHECKED_STATUS GetRegistration(
      ServerRegistrationPB* reg, RpcOnly rpc_only = RpcOnly::kFalse) const;

 protected:
  RpcAndWebServerBase(
      std::string name, const ServerBaseOptions& options,
      const std::string& metrics_namespace,
      std::shared_ptr<MemTracker> mem_tracker);
  virtual ~RpcAndWebServerBase();

  virtual Status HandleDebugPage(const Webserver::WebRequest& req, Webserver::WebResponse* resp);

  virtual void DisplayGeneralInfoIcons(std::stringstream* output);

  virtual CHECKED_STATUS DisplayRpcIcons(std::stringstream* output);

  static void DisplayIconTile(std::stringstream* output, const string icon, const string caption,
                              const string url);

  CHECKED_STATUS Init();
  CHECKED_STATUS Start();
  void Shutdown();

  gscoped_ptr<FsManager> fs_manager_;
  gscoped_ptr<Webserver> web_server_;

 private:
  void GenerateInstanceID();
  std::string GetEasterEggMessage() const;
  std::string FooterHtml() const;

  DISALLOW_COPY_AND_ASSIGN(RpcAndWebServerBase);
};

std::shared_ptr<MemTracker> CreateMemTrackerForServer();

YB_STRONGLY_TYPED_BOOL(Private);

// Returns public/private address for test server with specified index.
std::string TEST_RpcAddress(int index, Private priv);
// Returns bind endpoint for test server with specified index and specified port.
std::string TEST_RpcBindEndpoint(int index, uint16_t port);

// Sets up connectivity in test for specified messenger of server with index.
void TEST_SetupConnectivity(rpc::Messenger* messenger, int index);
// Isolates specific messenger, i.e. breaks connectivity with all other servers.
void TEST_Isolate(rpc::Messenger* messenger);

} // namespace server
} // namespace yb

#endif /* YB_SERVER_SERVER_BASE_H */
