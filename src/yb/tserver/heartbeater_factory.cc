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

#include "yb/tserver/heartbeater_factory.h"

#include "yb/tserver/tablet_split_heartbeat_data_provider.h"
#include "yb/tserver/tserver_metrics_heartbeat_data_provider.h"

namespace yb {
namespace tserver {

std::unique_ptr<Heartbeater> CreateHeartbeater(
    const TabletServerOptions& options, TabletServer* server) {
  std::vector<std::unique_ptr<HeartbeatDataProvider>> data_providers;
  data_providers.push_back(
      std::make_unique<TServerMetricsHeartbeatDataProvider>(server));
  data_providers.push_back(
      std::make_unique<TabletSplitHeartbeatDataProvider>(server));
  return std::make_unique<Heartbeater>(options, server, std::move(data_providers));
}

} // namespace tserver
} // namespace yb
