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

#include "yb/yql/redis/redisserver/redis_server.h"

#include "yb/gutil/strings/substitute.h"
#include "yb/yql/redis/redisserver/redis_rpc.h"
#include "yb/yql/redis/redisserver/redis_service.h"

#include "yb/util/flag_tags.h"
#include "yb/util/size_literals.h"

using yb::rpc::ServiceIf;
using namespace yb::size_literals;

DEFINE_int32(redis_svc_queue_length, 5000,
             "RPC queue length for redis service");
TAG_FLAG(redis_svc_queue_length, advanced);

DEFINE_int64(redis_rpc_block_size, 1_MB, "Redis RPC block size");
DEFINE_int64(redis_rpc_memory_limit, 0, "Redis RPC memory limit");

namespace yb {
namespace redisserver {

RedisServer::RedisServer(const RedisServerOptions& opts, const tserver::TabletServer* tserver)
    : RpcAndWebServerBase(
          "RedisServer", opts, "yb.redisserver",
          MemTracker::CreateTracker(
              "Redis", tserver ? tserver->mem_tracker() : MemTracker::GetRootTracker(),
              AddToParent::kTrue, CreateMetrics::kFalse)),
      opts_(opts),
      tserver_(tserver) {
  SetConnectionContextFactory(rpc::CreateConnectionContextFactory<RedisConnectionContext>(
      FLAGS_redis_rpc_block_size, FLAGS_redis_rpc_memory_limit, mem_tracker()->parent()));
}

Status RedisServer::Start() {
  RETURN_NOT_OK(server::RpcAndWebServerBase::Init());

  std::unique_ptr<ServiceIf> redis_service(new RedisServiceImpl(this, opts_.master_addresses_flag));
  RETURN_NOT_OK(RegisterService(FLAGS_redis_svc_queue_length, std::move(redis_service)));

  RETURN_NOT_OK(server::RpcAndWebServerBase::Start());

  return Status::OK();
}

}  // namespace redisserver
}  // namespace yb
