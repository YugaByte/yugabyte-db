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

#include "yb/yql/redis/redisserver/redis_service.h"

#include <iostream>
#include <thread>

#include <boost/algorithm/string/case_conv.hpp>

#include <boost/lockfree/queue.hpp>

#include <boost/logic/tribool.hpp>

#include <gflags/gflags.h>

#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/client/async_rpc.h"
#include "yb/client/callbacks.h"
#include "yb/client/client.h"
#include "yb/client/client_builder-internal.h"
#include "yb/client/client-internal.h"
#include "yb/client/error.h"
#include "yb/client/meta_data_cache.h"
#include "yb/client/meta_cache.h"
#include "yb/client/session.h"
#include "yb/client/table.h"
#include "yb/client/yb_op.h"

#include "yb/common/redis_protocol.pb.h"

#include "yb/yql/redis/redisserver/redis_commands.h"
#include "yb/yql/redis/redisserver/redis_constants.h"
#include "yb/yql/redis/redisserver/redis_encoding.h"
#include "yb/yql/redis/redisserver/redis_parser.h"
#include "yb/yql/redis/redisserver/redis_rpc.h"

#include "yb/rpc/connection.h"
#include "yb/rpc/rpc_context.h"
#include "yb/rpc/rpc_introspection.pb.h"

#include "yb/tserver/tablet_server.h"
#include "yb/tserver/tserver_service.proxy.h"

#include "yb/util/bytes_formatter.h"
#include "yb/util/locks.h"
#include "yb/util/logging.h"
#include "yb/util/memory/mc_types.h"
#include "yb/util/redis_util.h"
#include "yb/util/size_literals.h"
#include "yb/util/stol_utils.h"
#include "yb/util/shared_lock.h"

using yb::operator"" _MB;
using namespace std::literals;
using namespace std::placeholders;
using yb::client::YBMetaDataCache;
using strings::Substitute;
using yb::rpc::Connection;

DEFINE_REDIS_histogram_EX(error,
                          "yb.redisserver.RedisServerService.AnyMethod RPC Time",
                          "yb.redisserver.RedisServerService.ErrorUnsupportedMethod()");
DEFINE_REDIS_histogram_EX(get_internal,
                          "yb.redisserver.RedisServerService.Get RPC Time",
                          "in yb.client.Get");
DEFINE_REDIS_histogram_EX(set_internal,
                          "yb.redisserver.RedisServerService.Set RPC Time",
                          "in yb.client.Set");

#define DEFINE_REDIS_SESSION_GAUGE(state) \
  METRIC_DEFINE_gauge_uint64( \
      server, \
      BOOST_PP_CAT(redis_, BOOST_PP_CAT(state, _sessions)), \
      "Number of " BOOST_PP_STRINGIZE(state) " sessions", \
      yb::MetricUnit::kUnits, \
      "Number of sessions " BOOST_PP_STRINGIZE(state) " by Redis service.") \
      /**/

DEFINE_REDIS_SESSION_GAUGE(allocated);
DEFINE_REDIS_SESSION_GAUGE(available);

METRIC_DEFINE_gauge_uint64(
    server, redis_monitoring_clients, "Number of clients running monitor", yb::MetricUnit::kUnits,
    "Number of clients running monitor ");

#if defined(THREAD_SANITIZER) || defined(ADDRESS_SANITIZER)
constexpr int32_t kDefaultRedisServiceTimeoutMs = 600000;
#else
constexpr int32_t kDefaultRedisServiceTimeoutMs = 3000;
#endif

DEFINE_int32(redis_service_yb_client_timeout_millis, kDefaultRedisServiceTimeoutMs,
             "Timeout in milliseconds for RPC calls from Redis service to master/tserver");

// In order to support up to three 64MB strings along with other strings,
// we have the total size of a redis command at 253_MB, which is less than the consensus size
// to account for the headers in the consensus layer.
DEFINE_int32(redis_max_command_size, 253_MB,
             "Maximum size of the command in redis");

// Maximum value size is 64MB
DEFINE_int32(redis_max_value_size, 64_MB,
             "Maximum size of the value in redis");
DEFINE_int32(redis_callbacks_threadpool_size, 64,
             "The maximum size for the threadpool which handles callbacks from the ybclient layer");

DEFINE_int32(redis_password_caching_duration_ms, 5000,
             "The duration for which we will cache the redis passwords. 0 to disable.");

DEFINE_bool(redis_safe_batch, true, "Use safe batching with Redis service");
DEFINE_bool(enable_redis_auth, true, "Enable AUTH for the Redis service");

DECLARE_string(placement_cloud);
DECLARE_string(placement_region);
DECLARE_string(placement_zone);

using yb::client::YBRedisOp;
using yb::client::YBRedisReadOp;
using yb::client::YBRedisWriteOp;
using yb::client::YBClientBuilder;
using yb::client::YBSchema;
using yb::client::YBSession;
using yb::client::YBStatusCallback;
using yb::client::YBTable;
using yb::client::YBTableName;
using yb::rpc::ConnectionPtr;
using yb::rpc::ConnectionWeakPtr;
using yb::rpc::OutboundData;
using yb::rpc::OutboundDataPtr;
using yb::RedisResponsePB;

namespace yb {
namespace redisserver {

typedef boost::container::small_vector_base<Slice> RedisKeyList;

namespace {

YB_DEFINE_ENUM(OperationType, (kNone)(kRead)(kWrite)(kLocal));

// Returns opposite operation type for specified type. Write for read, read for write.
OperationType Opposite(OperationType type) {
  switch (type) {
    case OperationType::kRead:
      return OperationType::kWrite;
    case OperationType::kWrite:
      return OperationType::kRead;
    case OperationType::kNone: FALLTHROUGH_INTENDED;
    case OperationType::kLocal:
      FATAL_INVALID_ENUM_VALUE(OperationType, type);
  }
  FATAL_INVALID_ENUM_VALUE(OperationType, type);
}

class Operation {
 public:
  template <class Op>
  Operation(const std::shared_ptr<RedisInboundCall>& call,
            size_t index,
            std::shared_ptr<Op> operation,
            const rpc::RpcMethodMetrics& metrics)
    : type_(std::is_same<Op, YBRedisReadOp>::value ? OperationType::kRead : OperationType::kWrite),
      call_(call),
      index_(index),
      operation_(std::move(operation)),
      metrics_(metrics),
      manual_response_(ManualResponse::kFalse) {
    auto status = operation_->GetPartitionKey(&partition_key_);
    if (!status.ok()) {
      Respond(status);
    }
  }

  Operation(const std::shared_ptr<RedisInboundCall>& call,
            size_t index,
            std::function<bool(client::YBSession*, const StatusFunctor&)> functor,
            std::string partition_key,
            const rpc::RpcMethodMetrics& metrics,
            ManualResponse manual_response)
    : type_(OperationType::kLocal),
      call_(call),
      index_(index),
      functor_(std::move(functor)),
      partition_key_(std::move(partition_key)),
      metrics_(metrics),
      manual_response_(manual_response) {
  }

  bool responded() const {
    return responded_.load(std::memory_order_acquire);
  }

  size_t index() const {
    return index_;
  }

  OperationType type() const {
    return type_;
  }

  const YBRedisOp& operation() const {
    return *operation_;
  }

  bool has_operation() const {
    return operation_ != nullptr;
  }

  size_t space_used_by_request() const {
    return operation_ ? operation_->space_used_by_request() : 0;
  }

  RedisResponsePB& response() {
    switch (type_) {
      case OperationType::kRead:
        return *down_cast<YBRedisReadOp*>(operation_.get())->mutable_response();
      case OperationType::kWrite:
        return *down_cast<YBRedisWriteOp*>(operation_.get())->mutable_response();
      case OperationType::kNone: FALLTHROUGH_INTENDED;
      case OperationType::kLocal:
        FATAL_INVALID_ENUM_VALUE(OperationType, type_);
    }
    FATAL_INVALID_ENUM_VALUE(OperationType, type_);
  }

  const rpc::RpcMethodMetrics& metrics() const {
    return metrics_;
  }

  const std::string& partition_key() const {
    return partition_key_;
  }

  void SetTablet(const client::internal::RemoteTabletPtr& tablet) {
    if (operation_) {
      operation_->SetTablet(tablet);
    }
    tablet_ = tablet;
  }

  void ResetTable(std::shared_ptr<client::YBTable> table) {
    if (operation_) {
      operation_->ResetTable(table);
    }
    tablet_.reset();
  }

  const client::internal::RemoteTabletPtr& tablet() const {
    return tablet_;
  }

  RedisInboundCall& call() const {
    return *call_;
  }

  void GetKeys(RedisKeyList* keys) const {
    if (FLAGS_redis_safe_batch) {
      keys->emplace_back(operation_ ? operation_->GetKey() : Slice());
    }
  }

  bool Apply(client::YBSession* session, const StatusFunctor& callback, bool* applied_operations) {
    // We should destroy functor after this call.
    // Because it could hold references to other objects.
    // So we more it to temp variable.
    auto functor = std::move(functor_);

    if (call_->aborted()) {
      Respond(STATUS(Aborted, ""));
      return false;
    }

    // Used for DebugSleep
    if (functor) {
      return functor(session, callback);
    }

    auto status = session->Apply(operation_);
    if (!status.ok()) {
      Respond(status);
      return false;
    }
    *applied_operations = true;
    return true;
  }

  void Respond(const Status& status) {
    responded_.store(true, std::memory_order_release);
    if (manual_response_) {
      return;
    }

    if (status.ok()) {
      if (operation_) {
        call_->RespondSuccess(index_, metrics_, &response());
      } else {
        RedisResponsePB resp;
        call_->RespondSuccess(index_, metrics_, &resp);
      }
    } else if ((type_ == OperationType::kRead || type_ == OperationType::kWrite) &&
               response().code() == RedisResponsePB_RedisStatusCode_SERVER_ERROR) {
      call_->Respond(index_, false, &response());
    } else {
      call_->RespondFailure(index_, status);
    }
  }

  std::string ToString() const {
    return Format("{ index: $0, operation: $1 }", index_, operation_);
  }

 private:
  OperationType type_;
  std::shared_ptr<RedisInboundCall> call_;
  size_t index_;
  std::shared_ptr<YBRedisOp> operation_;
  std::function<bool(client::YBSession*, const StatusFunctor&)> functor_;
  std::string partition_key_;
  rpc::RpcMethodMetrics metrics_;
  ManualResponse manual_response_;
  client::internal::RemoteTabletPtr tablet_;
  std::atomic<bool> responded_{false};
};

class SessionPool {
 public:
  void Init(client::YBClient* client,
            const scoped_refptr<MetricEntity>& metric_entity) {
    client_ = client;
    auto* proto = &METRIC_redis_allocated_sessions;
    allocated_sessions_metric_ = proto->Instantiate(metric_entity, 0);
    proto = &METRIC_redis_available_sessions;
    available_sessions_metric_ = proto->Instantiate(metric_entity, 0);
  }

  std::shared_ptr<client::YBSession> Take() {
    client::YBSession* result = nullptr;
    if (!queue_.pop(result)) {
      std::lock_guard<std::mutex> lock(mutex_);
      auto session = client_->NewSession();
      session->SetTimeout(
          MonoDelta::FromMilliseconds(FLAGS_redis_service_yb_client_timeout_millis));
      sessions_.push_back(session);
      allocated_sessions_metric_->IncrementBy(1);
      return session;
    }
    available_sessions_metric_->DecrementBy(1);
    return result->shared_from_this();
  }

  void Release(const std::shared_ptr<client::YBSession>& session) {
    available_sessions_metric_->IncrementBy(1);
    queue_.push(session.get());
  }
 private:
  client::YBClient* client_ = nullptr;
  std::mutex mutex_;
  std::vector<std::shared_ptr<client::YBSession>> sessions_;
  boost::lockfree::queue<client::YBSession*> queue_{30};
  scoped_refptr<AtomicGauge<uint64_t>> allocated_sessions_metric_;
  scoped_refptr<AtomicGauge<uint64_t>> available_sessions_metric_;
};

class Block;
typedef std::shared_ptr<Block> BlockPtr;

class Block : public std::enable_shared_from_this<Block> {
 public:
  typedef MCVector<Operation*> Ops;

  Block(const BatchContextPtr& context,
        Ops::allocator_type allocator,
        rpc::RpcMethodMetrics metrics_internal)
      : context_(context),
        ops_(allocator),
        metrics_internal_(std::move(metrics_internal)),
        start_(MonoTime::Now()) {
  }

  Block(const Block&) = delete;
  void operator=(const Block&) = delete;

  void AddOperation(Operation* operation) {
    ops_.push_back(operation);
  }

  void Launch(SessionPool* session_pool, bool allow_local_calls_in_curr_thread = true) {
    session_pool_ = session_pool;
    session_ = session_pool->Take();
    bool has_ok = false;
    bool applied_operations = false;
    // Supposed to be called only once.
    StatusFunctor callback = BlockCallback(shared_from_this());
    for (auto* op : ops_) {
      has_ok = op->Apply(session_.get(), callback, &applied_operations) || has_ok;
    }
    if (has_ok) {
      if (applied_operations) {
        // Allow local calls in this thread only if no one is waiting behind us.
        session_->set_allow_local_calls_in_curr_thread(
            allow_local_calls_in_curr_thread && this->next_ == nullptr);
        session_->FlushAsync(std::move(callback));
      }
    } else {
      Processed();
    }
  }

  BlockPtr SetNext(const BlockPtr& next) {
    BlockPtr result = std::move(next_);
    next_ = next;
    return result;
  }

  std::string ToString() const {
    return Format("{ ops: $0 context: $1 next: $2 }",
                  ops_, static_cast<void*>(context_.get()), next_);
  }

 private:
  class BlockCallback {
   public:
    explicit BlockCallback(BlockPtr block) : block_(std::move(block)) {}

    void operator()(const Status& status) {
      // Block context owns the arena upon which this block is created.
      // Done is going to free up block's reference to context. So, unless we ensure that
      // the context lives beyond the block_.reset() we might get an error while updating the
      // ref-count for the block_ (in the area of arena owned by the context).
      auto context = block_->context_;
      DCHECK(context != nullptr) << block_.get();
      block_->Done(status);
      block_.reset();
    }
   private:
    BlockPtr block_;
  };

  friend class BlockCallback;

  void Done(const Status& status) {
    MonoTime now = MonoTime::Now();
    metrics_internal_.handler_latency->Increment(now.GetDeltaSince(start_).ToMicroseconds());
    VLOG(3) << "Received status from call " << status.ToString(true);

    std::unordered_map<const client::YBOperation*, Status> op_errors;
    bool tablet_not_found = false;
    if (!status.ok()) {
      if (session_ != nullptr) {
        for (const auto& error : session_->GetAndClearPendingErrors()) {
          if (error->status().IsNotFound()) {
            tablet_not_found = true;
          }
          op_errors[&error->failed_op()] = std::move(error->status());
          YB_LOG_EVERY_N_SECS(WARNING, 1) << "Explicit error while inserting: "
                                          << error->status().ToString();
        }
      }
    }

    if (tablet_not_found && Retrying()) {
        // We will retry and not mark the ops as failed.
        return;
    }

    for (auto* op : ops_) {
      if (op->has_operation() && op_errors.find(&op->operation()) != op_errors.end()) {
        // Could check here for NotFound either.
        auto s = op_errors[&op->operation()];
        op->Respond(s);
      } else {
        op->Respond(Status::OK());
      }
    }

    Processed();
  }

  void Processed() {
    auto allow_local_calls_in_curr_thread = false;
    if (session_) {
      allow_local_calls_in_curr_thread = session_->allow_local_calls_in_curr_thread();
      session_pool_->Release(session_);
      session_.reset();
    }
    if (next_) {
      next_->Launch(session_pool_, allow_local_calls_in_curr_thread);
    }
    context_.reset();
  }

  bool Retrying() {
    auto old_table = context_->table();
    context_->CleanYBTableFromCache();

    const int kMaxRetries = 2;
    if (num_retries_ >= kMaxRetries) {
      VLOG(3) << "Not retrying because we are past kMaxRetries. num_retries_ = " << num_retries_
              << " kMaxRetries = " << kMaxRetries;
      return false;
    }
    num_retries_++;

    auto allow_local_calls_in_curr_thread = false;
    if (session_) {
      allow_local_calls_in_curr_thread = session_->allow_local_calls_in_curr_thread();
      session_pool_->Release(session_);
      session_.reset();
    }

    auto table = context_->table();
    if (!table || table->id() == old_table->id()) {
      VLOG(3) << "Not retrying because new table is : " << (table ? table->id() : "nullptr")
              << " old table was " << old_table->id();
      return false;
    }

    // Swap out ops with the newer version of the ops referring to the newly created table.
    for (auto* op : ops_) {
      op->ResetTable(context_->table());
    }
    Launch(session_pool_, allow_local_calls_in_curr_thread);
    VLOG(3) << " Retrying with table : " << table->id() << " old table was " << old_table->id();
    return true;
  }

 private:
  BatchContextPtr context_;
  Ops ops_;
  rpc::RpcMethodMetrics metrics_internal_;
  MonoTime start_;
  SessionPool* session_pool_;
  std::shared_ptr<client::YBSession> session_;
  BlockPtr next_;
  int num_retries_ = 1;
};

typedef std::array<rpc::RpcMethodMetrics, kOperationTypeMapSize> InternalMetrics;

struct BlockData {
  explicit BlockData(Arena* arena) : used_keys(UsedKeys::allocator_type(arena)) {}

  std::string ToString() const {
    return Format("{ used_keys: $0 block: $1 count: $2 }", used_keys, block, count);
  }

  typedef MCUnorderedSet<Slice, Slice::Hash> UsedKeys;
  UsedKeys used_keys;
  BlockPtr block;
  size_t count = 0;
};

class TabletOperations {
 public:
  explicit TabletOperations(Arena* arena)
      : read_data_(arena), write_data_(arena) {
  }

  BlockData& data(OperationType type) {
    switch (type) {
      case OperationType::kRead:
        return read_data_;
      case OperationType::kWrite:
        return write_data_;
      case OperationType::kNone: FALLTHROUGH_INTENDED;
      case OperationType::kLocal:
        FATAL_INVALID_ENUM_VALUE(OperationType, type);
    }
    FATAL_INVALID_ENUM_VALUE(OperationType, type);
  }

  void Done(SessionPool* session_pool, bool allow_local_calls_in_curr_thread) {
    if (flush_head_) {
      flush_head_->Launch(session_pool, allow_local_calls_in_curr_thread);
    } else {
      if (read_data_.block) {
        read_data_.block->Launch(session_pool, allow_local_calls_in_curr_thread);
      }
      if (write_data_.block) {
        write_data_.block->Launch(session_pool, allow_local_calls_in_curr_thread);
      }
    }
  }

  void Process(const BatchContextPtr& context,
               Arena* arena,
               Operation* operation,
               const InternalMetrics& metrics_internal) {
    auto type = operation->type();
    if (type == OperationType::kLocal) {
      ProcessLocalOperation(context, arena, operation, metrics_internal);
      return;
    }
    boost::container::small_vector<Slice, RedisClientCommand::static_capacity> keys;
    operation->GetKeys(&keys);
    CheckConflicts(type, keys);
    auto& data = this->data(type);
    if (!data.block) {
      ArenaAllocator<Block> alloc(arena);
      data.block = std::allocate_shared<Block>(
          alloc, context, alloc, metrics_internal[static_cast<size_t>(OperationType::kRead)]);
      if (last_conflict_type_ == OperationType::kLocal) {
        last_local_block_->SetNext(data.block);
        last_conflict_type_ = type;
      } else if (type == last_conflict_type_) {
        auto old_value = this->data(Opposite(type)).block->SetNext(data.block);
        if (old_value) {
          LOG(DFATAL) << "Opposite already had next block: "
                      << operation->call().serialized_request().ToDebugString();
        }
      }
    }
    data.block->AddOperation(operation);
    RememberKeys(type, &keys);
  }

  std::string ToString() const {
    return Format("{ read_data: $0 write_data: $1 flush_head: $2 last_local_block: $3 "
                  "last_conflict_type: $4 }",
                  read_data_, write_data_, flush_head_, last_local_block_, last_conflict_type_);
  }

 private:
  void ProcessLocalOperation(const BatchContextPtr& context,
                             Arena* arena,
                             Operation* operation,
                             const InternalMetrics& metrics_internal) {
    ArenaAllocator<Block> alloc(arena);
    auto block = std::allocate_shared<Block>(
        alloc, context, alloc, metrics_internal[static_cast<size_t>(OperationType::kLocal)]);
    switch (last_conflict_type_) {
      case OperationType::kNone:
        if (read_data_.block) {
          flush_head_ = read_data_.block;
          if (write_data_.block) {
            read_data_.block->SetNext(write_data_.block);
            write_data_.block->SetNext(block);
          } else {
            read_data_.block->SetNext(block);
          }
        } else if (write_data_.block) {
          flush_head_ = write_data_.block;
          write_data_.block->SetNext(block);
        } else {
          flush_head_ = block;
        }
        break;
      case OperationType::kRead:
        read_data_.block->SetNext(block);
        break;
      case OperationType::kWrite:
        write_data_.block->SetNext(block);
        break;
      case OperationType::kLocal:
        last_local_block_->SetNext(block);
        break;
    }
    read_data_.block = nullptr;
    read_data_.used_keys.clear();
    write_data_.block = nullptr;
    write_data_.used_keys.clear();
    last_local_block_ = block;
    last_conflict_type_ = OperationType::kLocal;
    block->AddOperation(operation);
  }

  void ConflictFound(OperationType type) {
    auto& data = this->data(type);
    auto& opposite_data = this->data(Opposite(type));

    switch (last_conflict_type_) {
      case OperationType::kNone:
        flush_head_ = opposite_data.block;
        opposite_data.block->SetNext(data.block);
        break;
      case OperationType::kWrite:
      case OperationType::kRead:
        data.block = nullptr;
        data.used_keys.clear();
        break;
      case OperationType::kLocal:
        last_local_block_->SetNext(data.block);
        break;
    }
    last_conflict_type_ = type;
  }

  void CheckConflicts(OperationType type, const RedisKeyList& keys) {
    if (last_conflict_type_ == type) {
      return;
    }
    if (last_conflict_type_ == OperationType::kLocal) {
      return;
    }
    auto& opposite = data(Opposite(type));
    bool conflict = false;
    for (const auto& key : keys) {
      if (opposite.used_keys.count(key)) {
        conflict = true;
        break;
      }
    }
    if (conflict) {
      ConflictFound(type);
    }
  }

  void RememberKeys(OperationType type, RedisKeyList* keys) {
    BlockData* dest;
    switch (type) {
      case OperationType::kRead:
        dest = &read_data_;
        break;
      case OperationType::kWrite:
        dest = &write_data_;
        break;
      case OperationType::kNone: FALLTHROUGH_INTENDED;
      case OperationType::kLocal:
        FATAL_INVALID_ENUM_VALUE(OperationType, type);
    }
    for (auto& key : *keys) {
      dest->used_keys.insert(std::move(key));
    }
  }

  BlockData read_data_;
  BlockData write_data_;
  BlockPtr flush_head_;
  BlockPtr last_local_block_;

  // Type of command that caused last conflict between reads and writes.
  OperationType last_conflict_type_ = OperationType::kNone;
};

YB_STRONGLY_TYPED_BOOL(IsMonitorMessage);

struct RedisServiceImplData : public RedisServiceData {
  RedisServiceImplData(RedisServer* server, string&& yb_tier_master_addresses);

  void AppendToMonitors(Connection* conn) override;
  void RemoveFromMonitors(Connection* conn) override;
  void LogToMonitors(const string& end, const string& db, const RedisClientCommand& cmd) override;
  yb::Result<std::shared_ptr<client::YBTable>> GetYBTableForDB(const string& db_name) override;

  void CleanYBTableFromCacheForDB(const string& table);

  void AppendToSubscribers(
      AsPattern type, const std::vector<std::string>& channels, rpc::Connection* conn,
      std::vector<int>* subs) override;
  void RemoveFromSubscribers(
      AsPattern type, const std::vector<std::string>& channels, rpc::Connection* conn,
      std::vector<int>* subs) override;
  void CleanUpSubscriptions(Connection* conn) override;
  int NumSubscribers(AsPattern type, const std::string& channel) override;
  std::unordered_set<std::string> GetSubscriptions(AsPattern type, rpc::Connection* conn) override;
  std::unordered_set<std::string> GetAllSubscriptions(AsPattern type) override;
  int Publish(const string& channel, const string& message);
  void ForwardToInterestedProxies(
      const string& channel, const string& message, const IntFunctor& f) override;
  int PublishToLocalClients(IsMonitorMessage mode, const string& channel, const string& message);
  Result<vector<HostPortPB>> GetServerAddrsForChannel(const string& channel);
  int NumSubscriptionsUnlocked(Connection* conn);

  CHECKED_STATUS GetRedisPasswords(vector<string>* passwords) override;
  CHECKED_STATUS Initialize();
  bool initialized() const { return initialized_.load(std::memory_order_relaxed); }

  // yb::Result<std::shared_ptr<client::YBTable>> GetYBTableForDB(const string& db_name);

  std::string yb_tier_master_addresses_;

  yb::rpc::RpcMethodMetrics metrics_error_;
  InternalMetrics metrics_internal_;

  // Mutex that protects the creation of client_ and populating db_to_opened_table_.
  std::mutex yb_mutex_;
  std::atomic<bool> initialized_;
  client::YBClient* client_ = nullptr;
  SessionPool session_pool_;
  std::unordered_map<std::string, std::shared_ptr<client::YBTable>> db_to_opened_table_;
  std::shared_ptr<client::YBMetaDataCache> tables_cache_;

  rw_semaphore pubsub_mutex_;
  std::unordered_map<std::string, std::unordered_set<Connection*>> channels_to_clients_;
  std::unordered_map<std::string, std::unordered_set<Connection*>> patterns_to_clients_;
  struct ClientSubscription {
    std::unordered_set<std::string> channels;
    std::unordered_set<std::string> patterns;
  };
  std::unordered_map<Connection*, ClientSubscription> clients_to_subscriptions_;

  std::unordered_set<Connection*> monitoring_clients_;
  scoped_refptr<AtomicGauge<uint64_t>> num_clients_monitoring_;

  std::mutex redis_password_mutex_;
  MonoTime redis_cached_password_validity_expiry_;
  vector<string> redis_cached_passwords_;

  RedisServer* server_ = nullptr;
};

class BatchContextImpl : public BatchContext {
 public:
  BatchContextImpl(
      const string& dbname, const std::shared_ptr<RedisInboundCall>& call,
      RedisServiceImplData* impl_data)
      : impl_data_(impl_data),
        db_name_(dbname),
        call_(call),
        consumption_(impl_data->server_->mem_tracker(), 0),
        operations_(&arena_),
        lookups_left_(0),
        tablets_(&arena_) {
  }

  virtual ~BatchContextImpl() {}

  const RedisClientCommand& command(size_t idx) const override {
    return call_->client_batch()[idx];
  }

  const std::shared_ptr<RedisInboundCall>& call() const override {
    return call_;
  }

  client::YBClient* client() const override {
    return impl_data_->client_;
  }

  RedisServiceImplData* service_data() override {
    return impl_data_;
  }

  const RedisServer* server() override {
    return impl_data_->server_;
  }

  void CleanYBTableFromCache() override {
    impl_data_->CleanYBTableFromCacheForDB(db_name_);
  }

  std::shared_ptr<client::YBTable> table() override {
    auto table = impl_data_->GetYBTableForDB(db_name_);
    if (!table.ok()) {
      return nullptr;
    }
    return *table;
  }

  void Commit() {
    Commit(1);
  }

  void Commit(int retries) {
    if (operations_.empty()) {
      return;
    }

    auto table = impl_data_->GetYBTableForDB(db_name_);
    if (!table.ok()) {
      for (auto& operation : operations_) {
        operation.Respond(table.status());
      }
    }
    auto deadline = CoarseMonoClock::Now() + FLAGS_redis_service_yb_client_timeout_millis * 1ms;
    lookups_left_.store(operations_.size(), std::memory_order_release);
    retry_lookups_.store(false, std::memory_order_release);
    for (auto& operation : operations_) {
      impl_data_->client_->LookupTabletByKey(
          table.get(), operation.partition_key(), deadline,
          std::bind(
              &BatchContextImpl::LookupDone, scoped_refptr<BatchContextImpl>(this), &operation,
              retries, _1));
    }
  }

  void Apply(
      size_t index,
      std::shared_ptr<client::YBRedisReadOp> operation,
      const rpc::RpcMethodMetrics& metrics) override {
    DoApply(index, std::move(operation), metrics);
  }

  void Apply(
      size_t index,
      std::shared_ptr<client::YBRedisWriteOp> operation,
      const rpc::RpcMethodMetrics& metrics) override {
    DoApply(index, std::move(operation), metrics);
  }

  void Apply(
      size_t index,
      std::function<bool(client::YBSession*, const StatusFunctor&)> functor,
      std::string partition_key,
      const rpc::RpcMethodMetrics& metrics,
      ManualResponse manual_response) override {
    DoApply(index, std::move(functor), std::move(partition_key), metrics, manual_response);
  }

  std::string ToString() const {
    return Format("{ tablets: $0 }", tablets_);
  }

 private:
  template <class... Args>
  void DoApply(Args&&... args) {
    operations_.emplace_back(call_, std::forward<Args>(args)...);
    if (PREDICT_FALSE(operations_.back().responded())) {
      operations_.pop_back();
    } else {
      consumption_.Add(operations_.back().space_used_by_request());
    }
  }

  void LookupDone(
      Operation* operation, int retries, const Result<client::internal::RemoteTabletPtr>& result) {
    const int kMaxRetries = 2;
    if (!result.ok()) {
      auto status = result.status();
      if (status.IsNotFound() && retries < kMaxRetries) {
        retry_lookups_.store(false, std::memory_order_release);
      } else {
        operation->Respond(status);
      }
    } else {
      operation->SetTablet(*result);
    }
    if (lookups_left_.fetch_sub(1, std::memory_order_acq_rel) != 1) {
      return;
    }

    if (retries < kMaxRetries && retry_lookups_.load(std::memory_order_acquire)) {
      CleanYBTableFromCache();
      Commit(retries + 1);
      return;
    }

    BatchContextPtr self(this);
    for (auto& operation : operations_) {
      if (!operation.responded()) {
        auto it = tablets_.find(operation.tablet()->tablet_id());
        if (it == tablets_.end()) {
          it = tablets_.emplace(operation.tablet()->tablet_id(), TabletOperations(&arena_)).first;
        }
        it->second.Process(self, &arena_, &operation, impl_data_->metrics_internal_);
      }
    }

    int idx = 0;
    for (auto& tablet : tablets_) {
      tablet.second.Done(&impl_data_->session_pool_, ++idx == tablets_.size());
    }
    tablets_.clear();
  }

  RedisServiceImplData* impl_data_ = nullptr;

  const string db_name_;
  std::shared_ptr<RedisInboundCall> call_;
  ScopedTrackedConsumption consumption_;

  Arena arena_;
  MCDeque<Operation> operations_;
  std::atomic<bool> retry_lookups_;
  std::atomic<size_t> lookups_left_;
  MCUnorderedMap<Slice, TabletOperations, Slice::Hash> tablets_;
};

} // namespace

class RedisServiceImpl::Impl {
 public:
  Impl(RedisServer* server, string yb_tier_master_address);

  ~Impl() {
    // Wait for DebugSleep to finish.
    // We use DebugSleep only during tests.
    // So just for long enough, giving extra 400ms for it.
    std::this_thread::sleep_for(500ms);
  }

  void Handle(yb::rpc::InboundCallPtr call_ptr);

 private:
  void SetupMethod(const RedisCommandInfo& info) {
    auto info_ptr = std::make_shared<RedisCommandInfo>(info);
    std::string lower_name = boost::to_lower_copy(info.name);
    std::string upper_name = boost::to_upper_copy(info.name);
    size_t len = info.name.size();
    std::string temp(len, 0);
    for (size_t i = 0; i != (1ULL << len); ++i) {
      for (size_t j = 0; j != len; ++j) {
        temp[j] = i & (1 << j) ? upper_name[j] : lower_name[j];
      }
      names_.push_back(temp);
      CHECK(command_name_to_info_map_.emplace(names_.back(), info_ptr).second);
    }
  }

  bool CheckArgumentSizeOK(const RedisClientCommand& cmd_args) {
    for (Slice arg : cmd_args) {
      if (arg.size() > FLAGS_redis_max_value_size) {
        return false;
      }
    }
    return true;
  }

  bool CheckAuthentication(RedisConnectionContext* conn_context) {
    if (!conn_context->is_authenticated()) {
      vector<string> passwords;
      Status s = data_.GetRedisPasswords(&passwords);
      conn_context->set_authenticated(!FLAGS_enable_redis_auth || (s.ok() && passwords.empty()));
    }
    return conn_context->is_authenticated();
  }

  void PopulateHandlers();
  // Fetches the appropriate handler for the command, nullptr if none exists.
  const RedisCommandInfo* FetchHandler(const RedisClientCommand& cmd_args);

  std::deque<std::string> names_;
  std::unordered_map<Slice, RedisCommandInfoPtr, Slice::Hash> command_name_to_info_map_;

  RedisServiceImplData data_;
};

RedisServiceImplData::RedisServiceImplData(RedisServer* server, string&& yb_tier_master_addresses)
    : yb_tier_master_addresses_(std::move(yb_tier_master_addresses)),
      initialized_(false),
      server_(server) {}

yb::Result<std::shared_ptr<client::YBTable>> RedisServiceImplData::GetYBTableForDB(
    const string& db_name) {
  std::shared_ptr<client::YBTable> table;
  YBTableName table_name = GetYBTableNameForRedisDatabase(db_name);
  bool was_cached = false;
  auto res = tables_cache_->GetTable(table_name, &table, &was_cached);
  if (!res.ok()) return res;
  return table;
}

void RedisServiceImplData::AppendToMonitors(Connection* conn) {
  VLOG(3) << "AppendToMonitors (" << conn->ToString();
  {
    boost::lock_guard<decltype(pubsub_mutex_)> lock(pubsub_mutex_);
    monitoring_clients_.insert(conn);
    num_clients_monitoring_->set_value(monitoring_clients_.size());
  }
  auto& context = static_cast<RedisConnectionContext&>(conn->context());
  if (context.ClientMode() != RedisClientMode::kMonitoring) {
    context.SetClientMode(RedisClientMode::kMonitoring);
    context.SetCleanupHook(std::bind(&RedisServiceImplData::RemoveFromMonitors, this, conn));
  }
}

void RedisServiceImplData::RemoveFromMonitors(Connection* conn) {
  VLOG(3) << "RemoveFromMonitors (" << conn->ToString();
  {
    boost::lock_guard<decltype(pubsub_mutex_)> lock(pubsub_mutex_);
    monitoring_clients_.erase(conn);
    num_clients_monitoring_->set_value(monitoring_clients_.size());
  }
}

int RedisServiceImplData::NumSubscriptionsUnlocked(Connection* conn) {
  return clients_to_subscriptions_[conn].channels.size() +
         clients_to_subscriptions_[conn].patterns.size();
}

void RedisServiceImplData::AppendToSubscribers(
    AsPattern type, const std::vector<std::string>& channels, rpc::Connection* conn,
    std::vector<int>* subs) {
  boost::lock_guard<decltype(pubsub_mutex_)> lock(pubsub_mutex_);
  subs->clear();
  for (const auto& channel : channels) {
    VLOG(3) << "AppendToSubscribers (" << type << ", " << channel << ", " << conn->ToString();
    if (type == AsPattern::kTrue) {
      patterns_to_clients_[channel].insert(conn);
      clients_to_subscriptions_[conn].patterns.insert(channel);
    } else {
      channels_to_clients_[channel].insert(conn);
      clients_to_subscriptions_[conn].channels.insert(channel);
    }
    subs->push_back(NumSubscriptionsUnlocked(conn));
  }
  auto& context = static_cast<RedisConnectionContext&>(conn->context());
  if (context.ClientMode() != RedisClientMode::kSubscribed) {
    context.SetClientMode(RedisClientMode::kSubscribed);
    context.SetCleanupHook(std::bind(&RedisServiceImplData::CleanUpSubscriptions, this, conn));
  }
}

void RedisServiceImplData::RemoveFromSubscribers(
    AsPattern type, const std::vector<std::string>& channels, rpc::Connection* conn,
    std::vector<int>* subs) {
  boost::lock_guard<decltype(pubsub_mutex_)> lock(pubsub_mutex_);
  auto& map_to_clients = (type == AsPattern::kTrue ? patterns_to_clients_ : channels_to_clients_);
  auto& map_from_clients =
      (type == AsPattern::kTrue ? clients_to_subscriptions_[conn].patterns
                                : clients_to_subscriptions_[conn].channels);

  subs->clear();
  for (const auto& channel : channels) {
    map_to_clients[channel].erase(conn);
    if (map_to_clients[channel].empty()) {
      map_to_clients.erase(channel);
    }
    map_from_clients.erase(channel);
    subs->push_back(NumSubscriptionsUnlocked(conn));
  }
}

std::unordered_set<string> RedisServiceImplData::GetSubscriptions(
    AsPattern type, Connection* conn) {
  SharedLock<decltype(pubsub_mutex_)> lock(pubsub_mutex_);
  return (
      type == AsPattern::kTrue ? clients_to_subscriptions_[conn].patterns
                               : clients_to_subscriptions_[conn].channels);
}

// ENG-4199: Consider getting all the cluster-wide subscriptions?
std::unordered_set<string> RedisServiceImplData::GetAllSubscriptions(AsPattern type) {
  unordered_set<string> ret;
  SharedLock<decltype(pubsub_mutex_)> lock(pubsub_mutex_);
  for (const auto& element :
       (type == AsPattern::kTrue ? patterns_to_clients_ : channels_to_clients_)) {
    ret.insert(element.first);
  }
  return ret;
}

// ENG-4199: Consider getting all the cluster-wide subscribers?
int RedisServiceImplData::NumSubscribers(AsPattern type, const std::string& channel) {
  SharedLock<decltype(pubsub_mutex_)> lock(pubsub_mutex_);
  const auto& look_in = (type ? patterns_to_clients_ : channels_to_clients_);
  const auto& iter = look_in.find(channel);
  return (iter == look_in.end() ? 0 : iter->second.size());
}

void RedisServiceImplData::LogToMonitors(
    const string& end, const string& db, const RedisClientCommand& cmd) {
  {
    SharedLock<decltype(pubsub_mutex_)> rlock(pubsub_mutex_);
    if (monitoring_clients_.empty()) return;
  }

  // Prepare the string to be sent to all the monitoring clients.
  // TODO: Use timestamp that works with converter.
  int64_t now_ms = ToMicroseconds(CoarseMonoClock::Now().time_since_epoch());
  std::stringstream ss;
  ss << "+";
  ss.setf(std::ios::fixed, std::ios::floatfield);
  ss.precision(6);
  ss << (now_ms / 1000000.0) << " [" << db << " " << end << "]";
  for (auto& part : cmd) {
    ss << " \"" << part.ToBuffer() << "\"";
  }
  ss << "\r\n";

  PublishToLocalClients(IsMonitorMessage::kTrue, "", ss.str());
}

int RedisServiceImplData::Publish(const string& channel, const string& message) {
  VLOG(3) << "Forwarding to clients on channel " << channel;
  return PublishToLocalClients(IsMonitorMessage::kFalse, channel, message);
}

Result<vector<HostPortPB>> RedisServiceImplData::GetServerAddrsForChannel(
    const string& channel_unused) {
  // TODO(Amit): Instead of forwarding  blindly to all servers, figure out the
  // ones that have a subscription and send it to them only.
  std::vector<master::TSInformationPB> live_tservers;
  Status s = CHECK_NOTNULL(server_->tserver())->GetLiveTServers(&live_tservers);
  if (!s.ok()) {
    LOG(WARNING) << s;
    return s;
  }

  vector<HostPortPB> servers;
  const auto cloud_info_pb = server_->MakeCloudInfoPB();
  // Queue NEW_NODE event for all the live tservers.
  for (const master::TSInformationPB& ts_info : live_tservers) {
    const auto& hostport_pb = DesiredHostPort(ts_info.registration().common(), cloud_info_pb);
    if (hostport_pb.host().empty()) {
      LOG(WARNING) << "Skipping TS since it doesn't have any rpc address: "
                   << ts_info.DebugString();
      continue;
    }
    servers.push_back(hostport_pb);
  }
  return servers;
}

class PublishResponseHandler {
 public:
  PublishResponseHandler(int32_t n, IntFunctor f)
      : num_replies_pending(n), done_functor(std::move(f)) {}

  void HandleResponse(const tserver::PublishResponsePB* resp) {
    num_clients_forwarded_to.IncrementBy(resp->num_clients_forwarded_to());

    if (0 == num_replies_pending.IncrementBy(-1)) {
      done_functor(num_clients_forwarded_to.Load());
    }
  }

 private:
  AtomicInt<int32_t> num_replies_pending;
  AtomicInt<int32_t> num_clients_forwarded_to{0};
  IntFunctor done_functor;
};

void RedisServiceImplData::ForwardToInterestedProxies(
    const string& channel, const string& message, const IntFunctor& f) {
  auto interested_servers = GetServerAddrsForChannel(channel);
  if (!interested_servers.ok()) {
    LOG(ERROR) << "Could not get servers to forward to " << interested_servers.status();
    return;
  }
  std::shared_ptr<PublishResponseHandler> resp_handler =
      std::make_shared<PublishResponseHandler>(interested_servers->size(), f);
  for (auto& hostport_pb : *interested_servers) {
    tserver::PublishRequestPB requestPB;
    requestPB.set_channel(channel);
    requestPB.set_message(message);
    std::shared_ptr<tserver::TabletServerServiceProxy> proxy =
        std::make_shared<tserver::TabletServerServiceProxy>(
            &client_->proxy_cache(), HostPortFromPB(hostport_pb));
    std::shared_ptr<tserver::PublishResponsePB> responsePB =
        std::make_shared<tserver::PublishResponsePB>();
    std::shared_ptr<yb::rpc::RpcController> rpcController = std::make_shared<rpc::RpcController>();
    // Hold a copy of the shared ptr in the callback to ensure that the proxy, responsePB and
    // rpcController are valid.
    // these self-destruct on the latter of the two events
    //  (i)  exit this loop, and
    //  (ii) done with the callback.
    proxy->PublishAsync(
        requestPB, responsePB.get(), rpcController.get(),
        [resp_handler, responsePB, rpcController, proxy]() mutable {
          resp_handler->HandleResponse(responsePB.get());
          responsePB.reset();
          rpcController.reset();
          proxy.reset();
          resp_handler.reset();
        });
  }
}

string MessageFor(const string& channel, const string& message) {
  vector<string> parts;
  parts.push_back(redisserver::EncodeAsBulkString("message").ToBuffer());
  parts.push_back(redisserver::EncodeAsBulkString(channel).ToBuffer());
  parts.push_back(redisserver::EncodeAsBulkString(message).ToBuffer());
  return redisserver::EncodeAsArrayOfEncodedElements(parts);
}

string PMessageFor(const string& pattern, const string& channel, const string& message) {
  vector<string> parts;
  parts.push_back(redisserver::EncodeAsBulkString("pmessage").ToBuffer());
  parts.push_back(redisserver::EncodeAsBulkString(pattern).ToBuffer());
  parts.push_back(redisserver::EncodeAsBulkString(channel).ToBuffer());
  parts.push_back(redisserver::EncodeAsBulkString(message).ToBuffer());
  return redisserver::EncodeAsArrayOfEncodedElements(parts);
}

int RedisServiceImplData::PublishToLocalClients(
    IsMonitorMessage mode, const string& channel, const string& message) {
  SharedLock<decltype(pubsub_mutex_)> rlock(pubsub_mutex_);

  int num_pushed_to = 0;
  // Send the message to all the monitoring clients.
  OutboundDataPtr out;
  const std::unordered_set<Connection*>* clients = nullptr;
  if (mode == IsMonitorMessage::kTrue) {
    out = std::make_shared<yb::rpc::StringOutboundData>(message, "Monitor redis commands");
    clients = &monitoring_clients_;
  } else {
    out = std::make_shared<yb::rpc::StringOutboundData>(
        MessageFor(channel, message), "Publishing to Channel");
    clients =
        (channels_to_clients_.find(channel) == channels_to_clients_.end()
             ? nullptr
             : &channels_to_clients_[channel]);
  }
  if (clients) {
    // Handle Monitor and Subscribe clients.
    for (auto connection : *clients) {
      DVLOG(3) << "Publishing to subscribed client " << connection->ToString();
      connection->QueueOutboundData(out);
      num_pushed_to++;
    }
  }
  if (mode == IsMonitorMessage::kFalse) {
    // Handle PSubscribe clients.
    for (auto& entry : patterns_to_clients_) {
      auto& pattern = entry.first;
      auto& clients_subscribed_to_pattern = entry.second;
      if (!RedisUtil::RedisPatternMatch(pattern, channel, /* ignore case */ false)) {
        continue;
      }

      OutboundDataPtr out = std::make_shared<yb::rpc::StringOutboundData>(
          PMessageFor(pattern, channel, message), "Publishing to Channel");
      for (auto remote : clients_subscribed_to_pattern) {
        remote->QueueOutboundData(out);
        num_pushed_to++;
      }
    }
  }

  return num_pushed_to;
}

void RedisServiceImplData::CleanUpSubscriptions(Connection* conn) {
  VLOG(3) << "CleanUpSubscriptions (" << conn->ToString();
  boost::lock_guard<decltype(pubsub_mutex_)> wlock(pubsub_mutex_);
  if (monitoring_clients_.find(conn) != monitoring_clients_.end()) {
    monitoring_clients_.erase(conn);
    num_clients_monitoring_->set_value(monitoring_clients_.size());
  }
  if (clients_to_subscriptions_.find(conn) != clients_to_subscriptions_.end()) {
    for (auto& channel : clients_to_subscriptions_[conn].channels) {
      channels_to_clients_[channel].erase(conn);
      if (channels_to_clients_[channel].empty()) {
        channels_to_clients_.erase(channel);
      }
    }
    for (auto& pattern : clients_to_subscriptions_[conn].patterns) {
      patterns_to_clients_[pattern].erase(conn);
      if (patterns_to_clients_[pattern].empty()) {
        patterns_to_clients_.erase(pattern);
      }
    }
    clients_to_subscriptions_.erase(conn);
  }
}

Status RedisServiceImplData::Initialize() {
  boost::lock_guard<std::mutex> guard(yb_mutex_);
  if (!initialized()) {
    client_ = server_->tserver()->client();

    server_->tserver()->SetPublisher(std::bind(&RedisServiceImplData::Publish, this, _1, _2));

    tables_cache_ = std::make_shared<YBMetaDataCache>(
        client_, false /* Update roles permissions cache */);
    session_pool_.Init(client_, server_->metric_entity());

    initialized_.store(true, std::memory_order_release);
  }
  return Status::OK();
}

void RedisServiceImplData::CleanYBTableFromCacheForDB(const string& db) {
  tables_cache_->RemoveCachedTable(GetYBTableNameForRedisDatabase(db));
}

Status RedisServiceImplData::GetRedisPasswords(vector<string>* passwords) {
  MonoTime now = MonoTime::Now();

  std::lock_guard<std::mutex> lock(redis_password_mutex_);
  if (redis_cached_password_validity_expiry_.Initialized() &&
      now < redis_cached_password_validity_expiry_) {
    *passwords = redis_cached_passwords_;
    return Status::OK();
  }

  RETURN_NOT_OK(client_->GetRedisPasswords(&redis_cached_passwords_));
  *passwords = redis_cached_passwords_;
  redis_cached_password_validity_expiry_ =
      now + MonoDelta::FromMilliseconds(FLAGS_redis_password_caching_duration_ms);
  return Status::OK();
}

void RedisServiceImpl::Impl::PopulateHandlers() {
  auto metric_entity = data_.server_->metric_entity();
  FillRedisCommands(metric_entity, std::bind(&Impl::SetupMethod, this, _1));

  // Set up metrics for erroneous calls.
  data_.metrics_error_.handler_latency = YB_REDIS_METRIC(error).Instantiate(metric_entity);
  data_.metrics_internal_[static_cast<size_t>(OperationType::kWrite)].handler_latency =
      YB_REDIS_METRIC(set_internal).Instantiate(metric_entity);
  data_.metrics_internal_[static_cast<size_t>(OperationType::kRead)].handler_latency =
      YB_REDIS_METRIC(get_internal).Instantiate(metric_entity);
  data_.metrics_internal_[static_cast<size_t>(OperationType::kLocal)].handler_latency =
      data_.metrics_internal_[static_cast<size_t>(OperationType::kRead)].handler_latency;

  auto* proto = &METRIC_redis_monitoring_clients;
  data_.num_clients_monitoring_ = proto->Instantiate(metric_entity, 0);
}

const RedisCommandInfo* RedisServiceImpl::Impl::FetchHandler(const RedisClientCommand& cmd_args) {
  if (cmd_args.size() < 1) {
    return nullptr;
  }
  Slice cmd_name = cmd_args[0];
  auto iter = command_name_to_info_map_.find(cmd_args[0]);
  if (iter == command_name_to_info_map_.end()) {
    YB_LOG_EVERY_N_SECS(ERROR, 60)
        << "Command " << cmd_name << " not yet supported. "
        << "Arguments: " << ToString(cmd_args) << ". "
        << "Raw: " << Slice(cmd_args[0].data(), cmd_args.back().end()).ToDebugString();
    return nullptr;
  }
  return iter->second.get();
}

RedisServiceImpl::Impl::Impl(RedisServer* server, string yb_tier_master_addresses)
    : data_(server, std::move(yb_tier_master_addresses)) {
  PopulateHandlers();
}

bool AllowedInClientMode(const RedisCommandInfo* info, RedisClientMode mode) {
  if (mode == RedisClientMode::kMonitoring) {
    static std::unordered_set<string> allowed = {"quit"};
    return allowed.find(info->name) != allowed.end();
  } else if (mode == RedisClientMode::kSubscribed) {
    static std::unordered_set<string> allowed = {"subscribe",    "unsubscribe", "psubscribe",
                                                 "punsubscribe", "ping",        "quit"};
    return allowed.find(info->name) != allowed.end();
  } else {
    // kNormal.
    return true;
  }
}

void RedisServiceImpl::Impl::Handle(rpc::InboundCallPtr call_ptr) {
  auto call = std::static_pointer_cast<RedisInboundCall>(call_ptr);

  DVLOG(2) << "Asked to handle a call " << call->ToString();
  if (call->serialized_request().size() > FLAGS_redis_max_command_size) {
    auto message = StrCat("Size of redis command ", call->serialized_request().size(),
                          ", but we only support up to length of ", FLAGS_redis_max_command_size);
    for (size_t idx = 0; idx != call->client_batch().size(); ++idx) {
      RespondWithFailure(call, idx, message);
    }
    return;
  }

  // Ensure that we have the required YBClient(s) initialized.
  if (!data_.initialized()) {
    auto status = data_.Initialize();
    if (!status.ok()) {
      auto message = StrCat("Could not open .redis table. ", status.ToString());
      for (size_t idx = 0; idx != call->client_batch().size(); ++idx) {
        RespondWithFailure(call, idx, message);
      }
      return;
    }
  }

  // Call could contain several commands, i.e. batch.
  // We process them as follows:
  // Each read commands are processed individually.
  // Sequential write commands use single session and the same batcher.
  const auto& batch = call->client_batch();
  auto conn = call->connection();
  const string remote = yb::ToString(conn->remote());
  RedisConnectionContext* conn_context = &(call->connection_context());
  string db_name = conn_context->redis_db_to_use();
  auto context = make_scoped_refptr<BatchContextImpl>(db_name, call, &data_);
  for (size_t idx = 0; idx != batch.size(); ++idx) {
    const RedisClientCommand& c = batch[idx];

    auto cmd_info = FetchHandler(c);

    // Handle the current redis command.
    if (cmd_info == nullptr) {
      RespondWithFailure(call, idx, "Unsupported call.");
      continue;
    } else if (!AllowedInClientMode(cmd_info, conn_context->ClientMode())) {
      RespondWithFailure(
          call, idx, Substitute(
                         "Command $0 not allowed in client mode $1.", cmd_info->name,
                         yb::ToString(conn_context->ClientMode())));
      continue;
    }

    size_t arity = static_cast<size_t>(std::abs(cmd_info->arity) - 1);
    bool exact_count = cmd_info->arity > 0;
    size_t passed_arguments = c.size() - 1;
    if (!exact_count && passed_arguments < arity) {
      // -X means that the command needs >= X arguments.
      YB_LOG_EVERY_N_SECS(ERROR, 60)
          << "Requested command " << c[0] << " does not have enough arguments."
          << " At least " << arity << " expected, but " << passed_arguments << " found.";
      RespondWithFailure(call, idx, "Too few arguments.");
    } else if (exact_count && passed_arguments != arity) {
      // X (> 0) means that the command needs exactly X arguments.
      YB_LOG_EVERY_N_SECS(ERROR, 60)
          << "Requested command " << c[0] << " has wrong number of arguments. "
          << arity << " expected, but " << passed_arguments << " found.";
      RespondWithFailure(call, idx, "Wrong number of arguments.");
    } else if (!CheckArgumentSizeOK(c)) {
      RespondWithFailure(call, idx, "Redis argument too long.");
    } else if (!CheckAuthentication(conn_context) && cmd_info->name != "auth") {
      RespondWithFailure(call, idx, "Authentication required.", "NOAUTH");
    } else {
      if (cmd_info->name != "config" && cmd_info->name != "monitor") {
        data_.LogToMonitors(remote, db_name, c);
      }

      // Handle the call.
      cmd_info->functor(*cmd_info, idx, context.get());

      if (cmd_info->name == "select" && db_name != conn_context->redis_db_to_use()) {
        // update context.
        context->Commit();
        db_name = conn_context->redis_db_to_use();
        context = make_scoped_refptr<BatchContextImpl>(db_name, call, &data_);
      }
    }
  }
  context->Commit();
}

RedisServiceImpl::RedisServiceImpl(RedisServer* server, string yb_tier_master_address)
    : RedisServerServiceIf(server->metric_entity()),
      impl_(new Impl(server, std::move(yb_tier_master_address))) {}

RedisServiceImpl::~RedisServiceImpl() {
}

void RedisServiceImpl::Handle(yb::rpc::InboundCallPtr call) {
  impl_->Handle(std::move(call));
}

}  // namespace redisserver
}  // namespace yb
