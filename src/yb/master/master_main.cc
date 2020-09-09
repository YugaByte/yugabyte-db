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

#include <iostream>

#include <glog/logging.h>

#include "yb/gutil/strings/substitute.h"
#include "yb/master/call_home.h"
#include "yb/master/master.h"
#include "yb/consensus/log_util.h"
#include "yb/util/flags.h"
#include "yb/util/init.h"
#include "yb/util/logging.h"
#include "yb/util/main_util.h"
#include "yb/util/ulimit_util.h"
#include "yb/gutil/sysinfo.h"
#include "yb/server/total_mem_watcher.h"
#include "yb/util/net/net_util.h"

DECLARE_bool(callhome_enabled);
DECLARE_bool(evict_failed_followers);
DECLARE_double(default_memory_limit_to_ram_ratio);
DECLARE_int32(logbuflevel);
DECLARE_int32(webserver_port);
DECLARE_string(rpc_bind_addresses);
DECLARE_bool(durable_wal_write);
DECLARE_int32(stderrthreshold);

DECLARE_string(metric_node_name);
DECLARE_int64(remote_bootstrap_rate_limit_bytes_per_sec);
// Deprecated because it's misspelled.  But if set, this flag takes precedence over
// remote_bootstrap_rate_limit_bytes_per_sec for compatibility.
DECLARE_int64(remote_boostrap_rate_limit_bytes_per_sec);

using namespace std::literals;

namespace yb {
namespace master {

static int MasterMain(int argc, char** argv) {
  // Reset some default values before parsing gflags.
  FLAGS_rpc_bind_addresses = strings::Substitute("0.0.0.0:$0", kMasterDefaultPort);
  FLAGS_webserver_port = kMasterDefaultWebPort;

  string host_name;
  if (GetHostname(&host_name).ok()) {
    FLAGS_metric_node_name = strings::Substitute("$0:$1", host_name, kMasterDefaultWebPort);
  } else {
    LOG(INFO) << "Failed to get master's host name, keeping default metric_node_name";
  }

  FLAGS_default_memory_limit_to_ram_ratio = 0.10;
  // For masters we always want to fsync the WAL files.
  FLAGS_durable_wal_write = true;

  // A multi-node Master leader should not evict failed Master followers
  // because there is no-one to assign replacement servers in order to maintain
  // the desired replication factor. (It's not turtles all the way down!)
  FLAGS_evict_failed_followers = false;

  // Only write FATALs by default to stderr.
  FLAGS_stderrthreshold = google::FATAL;

  // Do not sync GLOG to disk for INFO, WARNING.
  // ERRORs, and FATALs will still cause a sync to disk.
  FLAGS_logbuflevel = google::GLOG_WARNING;
  ParseCommandLineFlags(&argc, &argv, true);
  if (argc != 1) {
    std::cerr << "usage: " << argv[0] << std::endl;
    return 1;
  }
  LOG_AND_RETURN_FROM_MAIN_NOT_OK(log::ModifyDurableWriteFlagIfNotODirect());
  LOG_AND_RETURN_FROM_MAIN_NOT_OK(InitYB(MasterOptions::kServerType, argv[0]));
  LOG(INFO) << "NumCPUs determined to be: " << base::NumCPUs();

  LOG_AND_RETURN_FROM_MAIN_NOT_OK(GetPrivateIpMode());

  auto opts_result = MasterOptions::CreateMasterOptions();
  LOG_AND_RETURN_FROM_MAIN_NOT_OK(opts_result);
  enterprise::Master server(*opts_result);

  if (FLAGS_remote_boostrap_rate_limit_bytes_per_sec > 0) {
    LOG(WARNING) << "Flag remote_boostrap_rate_limit_bytes_per_sec has been deprecated. "
                 << "Use remote_bootstrap_rate_limit_bytes_per_sec flag instead";
    FLAGS_remote_bootstrap_rate_limit_bytes_per_sec =
        FLAGS_remote_boostrap_rate_limit_bytes_per_sec;
  }

  SetDefaultInitialSysCatalogSnapshotFlags();

  // ==============================================================================================
  // End of setting master flags
  // ==============================================================================================

  LOG(INFO) << "Initializing master server...";
  LOG_AND_RETURN_FROM_MAIN_NOT_OK(server.Init());

  LOG(INFO) << "Starting Master server...";
  UlimitUtil::InitUlimits();
  LOG(INFO) << "ulimit cur(max)..." << UlimitUtil::GetUlimitInfo();
  LOG_AND_RETURN_FROM_MAIN_NOT_OK(server.Start());

  LOG(INFO) << "Master server successfully started.";

  std::unique_ptr<CallHome> call_home;
  if (FLAGS_callhome_enabled) {
    call_home = std::make_unique<CallHome>(&server, ServerType::MASTER);
    call_home->ScheduleCallHome();
  }

  auto total_mem_watcher = server::TotalMemWatcher::Create();
  total_mem_watcher->MemoryMonitoringLoop(
      [&server]() { server.Shutdown(); },
      [&server]() { return server.IsShutdown(); }
  );
  return EXIT_FAILURE;
}

} // namespace master
} // namespace yb

int main(int argc, char** argv) {
  return yb::master::MasterMain(argc, argv);
}
