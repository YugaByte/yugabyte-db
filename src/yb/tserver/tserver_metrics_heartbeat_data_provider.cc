// Copyright (c) YugaByte, Inc.
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

#include "yb/tserver/tserver_metrics_heartbeat_data_provider.h"

#include "yb/master/master.pb.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/util/logging.h"
#include "yb/util/mem_tracker.h"

DEFINE_int32(tserver_heartbeat_metrics_interval_ms, 5000,
             "Interval (in milliseconds) at which tserver sends its metrics in a heartbeat to "
             "master.");

using namespace std::literals;

namespace yb {
namespace tserver {

TServerMetricsHeartbeatDataProvider::TServerMetricsHeartbeatDataProvider(TabletServer* server) :
  PeriodicalHeartbeatDataProvider(server,
      MonoDelta::FromMilliseconds(FLAGS_tserver_heartbeat_metrics_interval_ms)),
  start_time_(MonoTime::Now()) {}

void TServerMetricsHeartbeatDataProvider::DoAddData(
    const master::TSHeartbeatResponsePB& last_resp, master::TSHeartbeatRequestPB* req) {
  // Get the total memory used.
  size_t mem_usage = MemTracker::GetRootTracker()->GetUpdatedConsumption(true /* force */);
  auto* metrics = req->mutable_metrics();
  metrics->set_total_ram_usage(static_cast<int64_t>(mem_usage));
  VLOG_WITH_PREFIX(4) << "Total Memory Usage: " << mem_usage;

  uint64_t total_file_sizes = 0;
  uint64_t uncompressed_file_sizes = 0;
  uint64_t num_files = 0;
  for (const auto& tablet_peer : server().tablet_manager()->GetTabletPeers()) {
    if (tablet_peer) {
      auto tablet = tablet_peer->shared_tablet();
      if (tablet) {
        total_file_sizes += tablet->GetCurrentVersionSstFilesSize();
        uncompressed_file_sizes += tablet->GetCurrentVersionSstFilesUncompressedSize();
        num_files += tablet->GetCurrentVersionNumSSTFiles();
      }
    }
  }
  metrics->set_total_sst_file_size(total_file_sizes);
  metrics->set_uncompressed_sst_file_size(uncompressed_file_sizes);
  metrics->set_num_sst_files(num_files);

  // Get the total number of read and write operations.
  auto reads_hist = server().GetMetricsHistogram(
      TabletServerServiceIf::RpcMetricIndexes::kMetricIndexRead);
  uint64_t num_reads = (reads_hist != nullptr) ? reads_hist->TotalCount() : 0;

  auto writes_hist = server().GetMetricsHistogram(
      TabletServerServiceIf::RpcMetricIndexes::kMetricIndexWrite);
  uint64_t num_writes = (writes_hist != nullptr) ? writes_hist->TotalCount() : 0;

  // Calculate the read and write ops per second.
  MonoDelta diff = CoarseMonoClock::Now() - prev_run_time();
  double_t div = diff.ToSeconds();

  double rops_per_sec = (div > 0 && num_reads > 0) ?
      (static_cast<double>(num_reads - prev_reads_) / div) : 0;

  double wops_per_sec = (div > 0 && num_writes > 0) ?
      (static_cast<double>(num_writes - prev_writes_) / div) : 0;

  prev_reads_ = num_reads;
  prev_writes_ = num_writes;
  metrics->set_read_ops_per_sec(rops_per_sec);
  metrics->set_write_ops_per_sec(wops_per_sec);
  uint64_t uptime_seconds = CalculateUptime();

  metrics->set_uptime_seconds(uptime_seconds);

  VLOG_WITH_PREFIX(4) << "Read Ops per second: " << rops_per_sec;
  VLOG_WITH_PREFIX(4) << "Write Ops per second: " << wops_per_sec;
  VLOG_WITH_PREFIX(4) << "Total SST File Sizes: "<< total_file_sizes;
  VLOG_WITH_PREFIX(4) << "Uptime seconds: "<< uptime_seconds;
}

uint64_t TServerMetricsHeartbeatDataProvider::CalculateUptime() {
  MonoDelta delta = MonoTime::Now().GetDeltaSince(start_time_);
  uint64_t uptime_seconds = static_cast<uint64_t>(delta.ToSeconds());
  return uptime_seconds;
}


} // namespace tserver
} // namespace yb
