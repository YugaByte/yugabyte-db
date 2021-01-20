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

#ifndef YB_CLIENT_CLIENT_UTILS_H
#define YB_CLIENT_CLIENT_UTILS_H

#include <future>

#include "yb/client/client_fwd.h"

#include "yb/rpc/rpc_fwd.h"

namespace yb {

class MemTracker;
class MetricEntity;

namespace client {

// Lookup first tablet of specified table.
std::future<Result<internal::RemoteTabletPtr>> LookupFirstTabletFuture(
    const std::shared_ptr<const YBTable>& table);

Result<std::unique_ptr<rpc::Messenger>> CreateClientMessenger(
    const string &client_name,
    int32_t num_reactors,
    const scoped_refptr<MetricEntity> &metric_entity,
    const std::shared_ptr<MemTracker> &parent_mem_tracker,
    rpc::SecureContext *secure_context = nullptr);

Result<std::vector<internal::RemoteTabletPtr>> FilterTabletsByHashPartitionKeyRange(
      const std::vector<internal::RemoteTabletPtr>& all_tablets,
      const std::string& partition_key_start,
      const std::string& partition_key_end);

} // namespace client
} // namespace yb

#endif // YB_CLIENT_CLIENT_UTILS_H
