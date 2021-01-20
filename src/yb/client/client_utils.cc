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

#include "yb/client/client_utils.h"

#include "yb/client/client.h"
#include "yb/client/client-internal.h"
#include "yb/client/meta_cache.h"
#include "yb/client/table.h"

#include "yb/rpc/secure_stream.h"

#include "yb/server/secure.h"

DECLARE_bool(TEST_running_test);

namespace yb {
namespace client {

constexpr int32_t kClientTimeoutSecs = 60;

std::future<Result<internal::RemoteTabletPtr>> LookupFirstTabletFuture(
    const std::shared_ptr<const YBTable>& table) {
  return table->client()->data_->meta_cache_->LookupTabletByKeyFuture(
      table, "" /* partition_key */,
      CoarseMonoClock::now() + std::chrono::seconds(kClientTimeoutSecs));
}

Result<std::unique_ptr<rpc::Messenger>> CreateClientMessenger(
    const string& client_name,
    int32_t num_reactors,
    const scoped_refptr<MetricEntity>& metric_entity,
    const std::shared_ptr<MemTracker>& parent_mem_tracker,
    rpc::SecureContext* secure_context) {
  rpc::MessengerBuilder builder(client_name);
  builder.set_num_reactors(num_reactors);
  builder.set_metric_entity(metric_entity);
  builder.UseDefaultConnectionContextFactory(parent_mem_tracker);
  if (secure_context) {
    server::ApplySecureContext(secure_context, &builder);
  }
  auto messenger = VERIFY_RESULT(builder.Build());
  if (PREDICT_FALSE(FLAGS_TEST_running_test)) {
    messenger->TEST_SetOutboundIpBase(VERIFY_RESULT(HostToAddress("127.0.0.1")));
  }
  return messenger;
}

Result<std::vector<internal::RemoteTabletPtr>> FilterTabletsByHashPartitionKeyRange(
    const std::vector<internal::RemoteTabletPtr>& all_tablets,
    const std::string& partition_key_start,
    const std::string& partition_key_end) {
  RETURN_NOT_OK(PartitionSchema::IsValidHashPartitionRange(partition_key_start,
                                                           partition_key_end));
  std::vector<internal::RemoteTabletPtr> filtered_results;
  for (const auto& remote_tablet : all_tablets) {
    auto tablet_partition_start = remote_tablet->partition().partition_key_start();
    auto tablet_partition_end = remote_tablet->partition().partition_key_end();
    // Is this tablet at the start
    bool start_condition = partition_key_start.empty() || tablet_partition_end.empty() ||
                           tablet_partition_end > partition_key_start;
    bool end_condition = partition_key_end.empty() || tablet_partition_start < partition_key_end;

    if (start_condition && end_condition) {
      filtered_results.push_back(remote_tablet);
    }
  }
  return filtered_results;
}


} // namespace client
} // namespace yb
