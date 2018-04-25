//
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
//

#ifndef YB_YQL_REDIS_REDISSERVER_REDIS_RPC_H
#define YB_YQL_REDIS_REDISSERVER_REDIS_RPC_H

#include <boost/container/small_vector.hpp>

#include "yb/yql/redis/redisserver/redis_fwd.h"
#include "yb/common/redis_protocol.pb.h"

#include "yb/rpc/connection_context.h"
#include "yb/rpc/rpc_with_queue.h"

namespace yb {

class MemTracker;

namespace redisserver {

class RedisParser;

class RedisConnectionContext : public rpc::ConnectionContextWithQueue {
 public:
  RedisConnectionContext(
      rpc::GrowableBufferAllocator* allocator,
      const MemTrackerPtr& call_tracker);
  ~RedisConnectionContext();

  static std::string Name() { return "Redis"; }

 private:
  void Connected(const rpc::ConnectionPtr& connection) override {}

  rpc::RpcConnectionPB::StateType State() override {
    return rpc::RpcConnectionPB::OPEN;
  }

  Result<size_t> ProcessCalls(const rpc::ConnectionPtr& connection,
                              const IoVecs& bytes_to_process,
                              rpc::ReadBufferFull read_buffer_full) override;
  size_t BufferLimit() override;

  // Takes ownership of data content.
  CHECKED_STATUS HandleInboundCall(const rpc::ConnectionPtr& connection,
                                   size_t commands_in_batch,
                                   std::vector<char>* data);

  std::unique_ptr<RedisParser> parser_;
  size_t commands_in_batch_ = 0;
  size_t end_of_batch_ = 0;

  MemTrackerPtr call_mem_tracker_;
};

class RedisInboundCall : public rpc::QueueableInboundCall {
 public:
  explicit RedisInboundCall(
     rpc::ConnectionPtr conn,
     size_t weight_in_bytes,
     CallProcessedListener call_processed_listener);

  ~RedisInboundCall();
  // Takes ownership of data content.
  CHECKED_STATUS ParseFrom(
      const MemTrackerPtr& mem_tracker, size_t commands, std::vector<char>* data);

  // Serialize the response packet for the finished call.
  // The resulting slices refer to memory in this object.
  void Serialize(std::deque<RefCntBuffer>* output) const override;

  void LogTrace() const override;
  std::string ToString() const override;
  bool DumpPB(const rpc::DumpRunningRpcsRequestPB& req, rpc::RpcCallInProgressPB* resp) override;

  MonoTime GetClientDeadline() const override;

  RedisClientBatch& client_batch() { return client_batch_; }

  const std::string& service_name() const override;
  const std::string& method_name() const override;
  void RespondFailure(rpc::ErrorStatusPB::RpcErrorCodePB error_code, const Status& status) override;

  void RespondFailure(size_t idx, const Status& status);
  void RespondSuccess(size_t idx,
                      const rpc::RpcMethodMetrics& metrics,
                      RedisResponsePB* resp);
  void MarkForClose() { quit_.store(true, std::memory_order_release); }

 private:
  void Respond(size_t idx, bool is_success, RedisResponsePB* resp);

  // The connection on which this inbound call arrived.
  static constexpr size_t batch_capacity = RedisClientBatch::static_capacity;
  boost::container::small_vector<RedisResponsePB, batch_capacity> responses_;
  boost::container::small_vector<std::atomic<size_t>, batch_capacity> ready_;
  std::atomic<size_t> ready_count_{0};
  std::atomic<bool> had_failures_{false};
  RedisClientBatch client_batch_;

  // Atomic bool to indicate if the command batch has been parsed.
  std::atomic<bool> parsed_ = {false};

  // Atomic bool to indicate if the quit command is present
  std::atomic<bool> quit_ = {false};

  ScopedTrackedConsumption consumption_;
};

} // namespace redisserver
} // namespace yb

#endif // YB_YQL_REDIS_REDISSERVER_REDIS_RPC_H
