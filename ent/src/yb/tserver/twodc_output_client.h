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

#include "yb/cdc/cdc_output_client_interface.h"
#include "yb/cdc/cdc_util.h"

#include "yb/rpc/rpc_fwd.h"

#ifndef ENT_SRC_YB_TSERVER_TWODC_OUTPUT_CLIENT_H
#define ENT_SRC_YB_TSERVER_TWODC_OUTPUT_CLIENT_H

namespace yb {

class ThreadPool;

namespace tserver {
namespace enterprise {

class CDCConsumer;
struct CDCClient;

std::unique_ptr<cdc::CDCOutputClient> CreateTwoDCOutputClient(
    CDCConsumer* cdc_consumer,
    const cdc::ConsumerTabletInfo& consumer_tablet_info,
    const std::shared_ptr<CDCClient>& local_client,
    rpc::Rpcs* rpcs,
    std::function<void(const cdc::OutputClientResponse& response)> apply_changes_clbk,
    bool use_local_tserver);

} // namespace enterprise
} // namespace tserver
} // namespace yb

#endif // ENT_SRC_YB_TSERVER_TWODC_OUTPUT_CLIENT_H
