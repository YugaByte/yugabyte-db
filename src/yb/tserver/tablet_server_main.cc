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

#include <chrono>
#include <iostream>

#include <boost/optional/optional.hpp>
#include "yb/tserver/tserver_error.h"
#include <glog/logging.h>

#ifdef TCMALLOC_ENABLED
#include <gperftools/malloc_extension.h>
#endif

#include "yb/yql/cql/cqlserver/cql_server.h"
#include "yb/yql/pgwrapper/pg_wrapper.h"
#include "yb/yql/redis/redisserver/redis_server.h"

#include "yb/consensus/log_util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/master/call_home.h"
#include "yb/rpc/io_thread_pool.h"
#include "yb/rpc/scheduler.h"
#include "yb/server/skewed_clock.h"
#include "yb/server/secure.h"
#include "yb/tserver/factory.h"
#include "yb/tserver/tablet_server.h"
#include "yb/util/flags.h"
#include "yb/util/init.h"
#include "yb/util/logging.h"
#include "yb/util/main_util.h"
#include "yb/util/ulimit_util.h"
#include "yb/util/size_literals.h"
#include "yb/util/net/net_util.h"

using namespace std::placeholders;

using yb::redisserver::RedisServer;
using yb::redisserver::RedisServerOptions;

using yb::cqlserver::CQLServer;
using yb::cqlserver::CQLServerOptions;

using yb::pgwrapper::PgProcessConf;
using yb::pgwrapper::PgWrapper;
using yb::pgwrapper::PgSupervisor;

using namespace yb::size_literals;  // NOLINT

DEFINE_bool(start_redis_proxy, true, "Starts a redis proxy along with the tablet server");

DEFINE_bool(start_cql_proxy, true, "Starts a CQL proxy along with the tablet server");
DEFINE_string(cql_proxy_broadcast_rpc_address, "",
              "RPC address to broadcast to other nodes. This is the broadcast_address used in the"
                  " system.local table");

DEFINE_int64(tserver_tcmalloc_max_total_thread_cache_bytes, 256_MB, "Total number of bytes to "
    "use for the thread cache for tcmalloc across all threads in the tserver.");

DECLARE_string(rpc_bind_addresses);
DECLARE_bool(callhome_enabled);
DECLARE_int32(webserver_port);
DECLARE_int32(logbuflevel);
DECLARE_int32(stderrthreshold);

DECLARE_string(redis_proxy_bind_address);
DECLARE_int32(redis_proxy_webserver_port);

DECLARE_string(cql_proxy_bind_address);
DECLARE_int32(cql_proxy_webserver_port);

DECLARE_string(pgsql_proxy_bind_address);
DECLARE_bool(start_pgsql_proxy);
DECLARE_bool(enable_ysql);

DECLARE_int64(remote_bootstrap_rate_limit_bytes_per_sec);

DECLARE_bool(use_client_to_server_encryption);
DECLARE_string(certs_dir);
DECLARE_string(certs_for_client_dir);
DECLARE_string(cert_node_filename);
DECLARE_string(ysql_hba_conf);
DECLARE_string(ysql_pg_conf);
DECLARE_string(metric_node_name);

// Deprecated because it's misspelled.  But if set, this flag takes precedence over
// remote_bootstrap_rate_limit_bytes_per_sec for compatibility.
DECLARE_int64(remote_boostrap_rate_limit_bytes_per_sec);

namespace yb {
namespace tserver {
namespace {

void SetProxyAddress(std::string* flag, const std::string& name, uint16_t port) {
  if (flag->empty()) {
    std::vector<HostPort> bind_addresses;
    Status status = HostPort::ParseStrings(FLAGS_rpc_bind_addresses, 0, &bind_addresses);
    LOG_IF(DFATAL, !status.ok()) << "Bad public IPs " << FLAGS_rpc_bind_addresses << ": " << status;
    if (!bind_addresses.empty()) {
      for (auto& addr : bind_addresses) {
        addr.set_port(port);
      }
      *flag = HostPort::ToCommaSeparatedString(bind_addresses);
      LOG(INFO) << "Reset " << name << " bind address to " << *flag;
    }
  }
}

// Helper function to set the proxy rpc addresses based on rpc_bind_addresses.
void SetProxyAddresses() {
  LOG(INFO) << "Using parsed rpc = " << FLAGS_rpc_bind_addresses;
  SetProxyAddress(&FLAGS_redis_proxy_bind_address, "YEDIS", RedisServer::kDefaultPort);
  SetProxyAddress(&FLAGS_cql_proxy_bind_address, "YCQL", CQLServer::kDefaultPort);
}

int TabletServerMain(int argc, char** argv) {
  // Reset some default values before parsing gflags.
  FLAGS_rpc_bind_addresses = strings::Substitute("0.0.0.0:$0",
                                                 TabletServer::kDefaultPort);
  FLAGS_webserver_port = TabletServer::kDefaultWebPort;
  FLAGS_redis_proxy_webserver_port = RedisServer::kDefaultWebPort;
  FLAGS_cql_proxy_webserver_port = CQLServer::kDefaultWebPort;

  string host_name;
  if (GetHostname(&host_name).ok()) {
    FLAGS_metric_node_name = strings::Substitute("$0:$1", host_name, TabletServer::kDefaultWebPort);
  } else {
    LOG(INFO) << "Failed to get tablet's host name, keeping default metric_node_name";
  }
  // Do not sync GLOG to disk for INFO, WARNING.
  // ERRORs, and FATALs will still cause a sync to disk.
  FLAGS_logbuflevel = google::GLOG_WARNING;

  server::SkewedClock::Register();

  // Only write FATALs by default to stderr.
  FLAGS_stderrthreshold = google::FATAL;

  ParseCommandLineFlags(&argc, &argv, true);
  if (argc != 1) {
    std::cerr << "usage: " << argv[0] << std::endl;
    return 1;
  }
  LOG_AND_RETURN_FROM_MAIN_NOT_OK(log::ModifyDurableWriteFlagIfNotODirect());
  LOG_AND_RETURN_FROM_MAIN_NOT_OK(InitYB(TabletServerOptions::kServerType, argv[0]));

  LOG(INFO) << "NumCPUs determined to be: " << base::NumCPUs();

  if (FLAGS_remote_boostrap_rate_limit_bytes_per_sec > 0) {
    LOG(WARNING) << "Flag remote_boostrap_rate_limit_bytes_per_sec has been deprecated. "
                 << "Use remote_bootstrap_rate_limit_bytes_per_sec flag instead";
    FLAGS_remote_bootstrap_rate_limit_bytes_per_sec =
        FLAGS_remote_boostrap_rate_limit_bytes_per_sec;
  }

#ifdef TCMALLOC_ENABLED
  LOG(INFO) << "Setting tcmalloc max thread cache bytes to: " <<
    FLAGS_tserver_tcmalloc_max_total_thread_cache_bytes;
  if (!MallocExtension::instance()->SetNumericProperty(kTcMallocMaxThreadCacheBytes,
      FLAGS_tserver_tcmalloc_max_total_thread_cache_bytes)) {
    LOG(FATAL) << "Failed to set Tcmalloc property: " << kTcMallocMaxThreadCacheBytes;
  }
#endif

  CHECK_OK(GetPrivateIpMode());

  SetProxyAddresses();

  auto tablet_server_options = TabletServerOptions::CreateTabletServerOptions();
  LOG_AND_RETURN_FROM_MAIN_NOT_OK(tablet_server_options);
  enterprise::Factory factory;

  auto server = factory.CreateTabletServer(*tablet_server_options);

  // ----------------------------------------------------------------------------------------------
  // Starting to instantiate servers
  // ----------------------------------------------------------------------------------------------

  LOG(INFO) << "Initializing tablet server...";
  LOG_AND_RETURN_FROM_MAIN_NOT_OK(server->Init());
  LOG(INFO) << "Starting tablet server...";
  UlimitUtil::InitUlimits();
  LOG(INFO) << "ulimit cur(max)..." << UlimitUtil::GetUlimitInfo();
  LOG_AND_RETURN_FROM_MAIN_NOT_OK(server->Start());
  LOG(INFO) << "Tablet server successfully started.";

  std::unique_ptr<CallHome> call_home;
  if (FLAGS_callhome_enabled) {
    call_home = std::make_unique<CallHome>(server.get(), ServerType::TSERVER);
    call_home->ScheduleCallHome();
  }

  std::unique_ptr<PgSupervisor> pg_supervisor;
  if (FLAGS_start_pgsql_proxy || FLAGS_enable_ysql) {
    auto pg_process_conf_result = PgProcessConf::CreateValidateAndRunInitDb(
        FLAGS_pgsql_proxy_bind_address,
        tablet_server_options->fs_opts.data_paths.front() + "/pg_data",
        server->GetSharedMemoryFd());
    LOG_AND_RETURN_FROM_MAIN_NOT_OK(pg_process_conf_result);
    auto& pg_process_conf = *pg_process_conf_result;
    pg_process_conf.master_addresses = tablet_server_options->master_addresses_flag;
    pg_process_conf.certs_dir = FLAGS_certs_dir.empty()
        ? server::DefaultCertsDir(*server->fs_manager())
        : FLAGS_certs_dir;
    pg_process_conf.certs_for_client_dir = FLAGS_certs_for_client_dir.empty()
        ? pg_process_conf.certs_dir
        : FLAGS_certs_for_client_dir;
    pg_process_conf.enable_tls = FLAGS_use_client_to_server_encryption;

    // Follow the same logic as elsewhere, check FLAGS_cert_node_filename then
    // server_broadcast_addresses then rpc_bind_addresses.
    if (!FLAGS_cert_node_filename.empty()) {
      pg_process_conf.cert_base_name = FLAGS_cert_node_filename;
    } else {
      const auto server_broadcast_addresses =
          HostPort::ParseStrings(server->options().server_broadcast_addresses, 0);
      LOG_AND_RETURN_FROM_MAIN_NOT_OK(server_broadcast_addresses);
      const auto rpc_bind_addresses =
          HostPort::ParseStrings(server->options().rpc_opts.rpc_bind_addresses, 0);
      LOG_AND_RETURN_FROM_MAIN_NOT_OK(rpc_bind_addresses);
      pg_process_conf.cert_base_name = !server_broadcast_addresses->empty()
                                     ? server_broadcast_addresses->front().host()
                                     : rpc_bind_addresses->front().host();
    }
    LOG(INFO) << "Starting PostgreSQL server listening on "
              << pg_process_conf.listen_addresses << ", port " << pg_process_conf.pg_port;

    pg_supervisor = std::make_unique<PgSupervisor>(pg_process_conf);
    LOG_AND_RETURN_FROM_MAIN_NOT_OK(pg_supervisor->Start());
  }

  std::unique_ptr<RedisServer> redis_server;
  if (FLAGS_start_redis_proxy) {
    RedisServerOptions redis_server_options;
    redis_server_options.rpc_opts.rpc_bind_addresses = FLAGS_redis_proxy_bind_address;
    redis_server_options.webserver_opts.port = FLAGS_redis_proxy_webserver_port;
    redis_server_options.master_addresses_flag = tablet_server_options->master_addresses_flag;
    redis_server_options.SetMasterAddresses(tablet_server_options->GetMasterAddresses());
    redis_server_options.dump_info_path =
        (tablet_server_options->dump_info_path.empty()
             ? ""
             : tablet_server_options->dump_info_path + "-redis");
    redis_server.reset(new RedisServer(redis_server_options, server.get()));
    LOG(INFO) << "Starting redis server...";
    LOG_AND_RETURN_FROM_MAIN_NOT_OK(redis_server->Start());
    LOG(INFO) << "Redis server successfully started.";
  }

  // TODO(neil): After CQL server is starting, it blocks this thread from moving on.
  // This should be fixed such that all processes or service by tablet server are treated equally
  // by using different threads for each process.
  std::unique_ptr<CQLServer> cql_server;
  if (FLAGS_start_cql_proxy) {
    CQLServerOptions cql_server_options;
    cql_server_options.rpc_opts.rpc_bind_addresses = FLAGS_cql_proxy_bind_address;
    cql_server_options.broadcast_rpc_address = FLAGS_cql_proxy_broadcast_rpc_address;
    cql_server_options.webserver_opts.port = FLAGS_cql_proxy_webserver_port;
    cql_server_options.master_addresses_flag = tablet_server_options->master_addresses_flag;
    cql_server_options.SetMasterAddresses(tablet_server_options->GetMasterAddresses());
    cql_server_options.dump_info_path =
        (tablet_server_options->dump_info_path.empty()
             ? ""
             : tablet_server_options->dump_info_path + "-cql");
    boost::asio::io_service io;
    cql_server = factory.CreateCQLServer(cql_server_options, &io, server.get());
    LOG(INFO) << "Starting CQL server...";
    LOG_AND_RETURN_FROM_MAIN_NOT_OK(cql_server->Start());
    LOG(INFO) << "CQL server successfully started.";

    // Should run forever unless there are some errors.
    boost::system::error_code ec;
    io.run(ec);
    if (ec) {
      LOG(WARNING) << "IO service run failure: " << ec;
    }

    LOG (WARNING) << "CQL Server shutting down";
    cql_server->Shutdown();
  }

  while (true) {
    SleepFor(MonoDelta::FromSeconds(60));
  }

  return 0;
}

}  // namespace
}  // namespace tserver
}  // namespace yb

int main(int argc, char** argv) {
  return yb::tserver::TabletServerMain(argc, argv);
}
