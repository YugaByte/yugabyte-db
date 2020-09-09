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

std::future<Result<internal::RemoteTabletPtr>> LookupFirstTabletFuture(const YBTable* table) {
  return table->client()->data_->meta_cache_->LookupTabletByKeyFuture(
      table, "" /* partition_key */, CoarseMonoClock::now() + std::chrono::seconds(60));
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


} // namespace client
} // namespace yb
