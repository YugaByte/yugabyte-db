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

#include "yb/yql/redis/redisserver/redis_commands.h"

#include <boost/algorithm/string.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/stringize.hpp>

#include <gflags/gflags.h>

#include "yb/client/client.h"
#include "yb/client/session.h"
#include "yb/client/table.h"
#include "yb/client/table_creator.h"
#include "yb/client/yb_op.h"

#include "yb/master/master.pb.h"
#include "yb/master/master_util.h"

#include "yb/rpc/connection.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/scheduler.h"

#include "yb/util/crypt.h"
#include "yb/util/metrics.h"
#include "yb/util/redis_util.h"
#include "yb/util/stol_utils.h"
#include "yb/util/string_util.h"

#include "yb/yql/redis/redisserver/redis_constants.h"
#include "yb/yql/redis/redisserver/redis_encoding.h"
#include "yb/yql/redis/redisserver/redis_rpc.h"

using namespace std::literals;
using namespace std::placeholders;
using yb::client::YBTableName;

namespace {
static bool ValidateRedisPasswordSeparator(const char* flagname, const string& value) {
  if (value.size() != 1) {
    LOG(INFO) << "Expect " << flagname << " to be 1 character long";
    return false;
  }
  return true;
}
}

DEFINE_bool(yedis_enable_flush, true, "Enables FLUSHDB and FLUSHALL commands in yedis.");
DEFINE_bool(use_hashed_redis_password, true, "Store the hash of the redis passwords instead.");
DEFINE_string(redis_passwords_separator, ",", "The character used to separate multiple passwords.");

DEFINE_int32(redis_keys_threshold, 10000,
             "Maximum number of keys allowed to be in the db before the KEYS operation errors out");

__attribute__((unused))
DEFINE_validator(redis_passwords_separator, &ValidateRedisPasswordSeparator);

namespace yb {
namespace redisserver {

#define REDIS_COMMANDS \
    ((get, Get, 2, READ)) \
    ((mget, MGet, -2, READ)) \
    ((hget, HGet, 3, READ)) \
    ((tsget, TsGet, 3, READ)) \
    ((hmget, HMGet, -3, READ)) \
    ((hgetall, HGetAll, 2, READ)) \
    ((hkeys, HKeys, 2, READ)) \
    ((hvals, HVals, 2, READ)) \
    ((hlen, HLen, 2, READ)) \
    ((hexists, HExists, 3, READ)) \
    ((hstrlen, HStrLen, 3, READ)) \
    ((smembers, SMembers, 2, READ)) \
    ((sismember, SIsMember, 3, READ)) \
    ((scard, SCard, 2, READ)) \
    ((strlen, StrLen, 2, READ)) \
    ((exists, Exists, 2, READ)) \
    ((getrange, GetRange, 4, READ)) \
    ((zcard, ZCard, 2, READ)) \
    ((rename, Rename, 3, LOCAL)) \
    ((set, Set, -3, WRITE)) \
    ((mset, MSet, -3, WRITE)) \
    ((hset, HSet, 4, WRITE)) \
    ((hmset, HMSet, -4, WRITE)) \
    ((hincrby, HIncrBy, 4, WRITE)) \
    ((hdel, HDel, -3, WRITE)) \
    ((sadd, SAdd, -3, WRITE)) \
    ((srem, SRem, -3, WRITE)) \
    ((tsadd, TsAdd, -4, WRITE)) \
    ((tsrangebytime, TsRangeByTime, 4, READ)) \
    ((tsrevrangebytime, TsRevRangeByTime, -4, READ)) \
    ((tslastn, TsLastN, 3, READ)) \
    ((tscard, TsCard, 2, READ)) \
    ((zrangebyscore, ZRangeByScore, -4, READ)) \
    ((zrevrange, ZRevRange, -4, READ)) \
    ((zrange, ZRange, -4, READ)) \
    ((zscore, ZScore, 3, READ)) \
    ((tsrem, TsRem, -3, WRITE)) \
    ((zrem, ZRem, -3, WRITE)) \
    ((zadd, ZAdd, -4, WRITE)) \
    ((getset, GetSet, 3, WRITE)) \
    ((append, Append, 3, WRITE)) \
    ((del, Del, 2, WRITE)) \
    ((setrange, SetRange, 4, WRITE)) \
    ((incr, Incr, 2, WRITE)) \
    ((incrby, IncrBy, 3, WRITE)) \
    ((echo, Echo, 2, LOCAL)) \
    ((auth, Auth, 2, LOCAL)) \
    ((config, Config, -1, LOCAL)) \
    ((info, Info, -1, LOCAL)) \
    ((role, Role, 1, LOCAL)) \
    ((select, Select, 2, LOCAL)) \
    ((createdb, CreateDB, 2, LOCAL)) \
    ((listdb, ListDB, 1, LOCAL)) \
    ((deletedb, DeleteDB, 2, LOCAL)) \
    ((ping, Ping, -1, LOCAL)) \
    ((command, Command, -1, LOCAL)) \
    ((monitor, Monitor, 1, LOCAL)) \
    ((pubsub, PubSub, -2, LOCAL)) \
    ((publish, Publish, 3, LOCAL)) \
    ((subscribe, Subscribe, -2, LOCAL)) \
    ((unsubscribe, Unsubscribe, -1, LOCAL)) \
    ((psubscribe, PSubscribe, -2, LOCAL)) \
    ((punsubscribe, PUnsubscribe, -1, LOCAL)) \
    ((quit, Quit, 1, LOCAL)) \
    ((flushdb, FlushDB, 1, LOCAL)) \
    ((flushall, FlushAll, 1, LOCAL)) \
    ((debugsleep, DebugSleep, 2, LOCAL)) \
    ((keys, Keys, 2, LOCAL)) \
    ((cluster, Cluster, -2, CLUSTER)) \
    ((persist, Persist, 2, WRITE)) \
    ((expire, Expire, 3, WRITE)) \
    ((pexpire, PExpire, 3, WRITE)) \
    ((expireat, ExpireAt, 3, WRITE))   \
    ((pexpireat, PExpireAt, 3, WRITE)) \
    ((ttl, Ttl, 2, READ)) \
    ((pttl, PTtl, 2, READ)) \
    ((setex, SetEx, 4, WRITE)) \
    ((psetex, PSetEx, 4, WRITE)) \
    ((lpop, LPop, 2, WRITE)) \
    ((lpush, LPush, -3, WRITE)) \
    ((rpop, RPop, 2, WRITE)) \
    ((rpush, RPush, -3, WRITE)) \
    ((llen, LLen, 2, READ)) \
    ((setnx, SetNX, 3, WRITE)) \
    /**/

#define DO_DEFINE_HISTOGRAM(name, cname, arity, type) \
  DEFINE_REDIS_histogram(name, BOOST_PP_STRINGIZE(cname));
#define DEFINE_HISTOGRAM(r, data, elem) DO_DEFINE_HISTOGRAM elem

BOOST_PP_SEQ_FOR_EACH(DEFINE_HISTOGRAM, ~, REDIS_COMMANDS)

#define READ_OP yb::client::YBRedisReadOp
#define WRITE_OP yb::client::YBRedisWriteOp
#define LOCAL_OP RedisResponsePB
#define CLUSTER_OP RedisResponsePB

#define DO_PARSER_FORWARD(name, cname, arity, type) \
    CHECKED_STATUS BOOST_PP_CAT(Parse, cname)( \
        BOOST_PP_CAT(type, _OP) *op, \
        const RedisClientCommand& args);
#define PARSER_FORWARD(r, data, elem) DO_PARSER_FORWARD elem

BOOST_PP_SEQ_FOR_EACH(PARSER_FORWARD, ~, REDIS_COMMANDS)

YBTableName RedisServiceData::GetYBTableNameForRedisDatabase(const string& db_name) {
  return YBTableName(YQL_DATABASE_REDIS,
                     common::kRedisKeyspaceName,
                     db_name == "0" ? string(common::kRedisTableName)
                                    : StrCat(common::kRedisTableName, "_", db_name));
}

namespace {

template<class Op>
using Parser = Status(*)(Op*, const RedisClientCommand&);

template<class Op>
void Command(
    const RedisCommandInfo& info,
    size_t idx,
    Parser<Op> parser,
    BatchContext* context) {
  VLOG(1) << "Processing " << info.name << ".";

  auto table = context->table();
  if (!table) {
    RespondWithFailure(context->call(), idx, "Could not open YBTable");
    return;
  }

  auto op = std::make_shared<Op>(table);
  const auto& command = context->command(idx);
  Status s = parser(op.get(), command);
  if (!s.ok()) {
    RespondWithFailure(context->call(), idx, s.message().ToBuffer());
    return;
  }
  context->Apply(idx, std::move(op), info.metrics);
}

#define READ_COMMAND(cname) \
    Command<yb::client::YBRedisReadOp>(info, idx, &BOOST_PP_CAT(Parse, cname), context)
#define WRITE_COMMAND(cname) \
    Command<yb::client::YBRedisWriteOp>(info, idx, &BOOST_PP_CAT(Parse, cname), context)
#define LOCAL_COMMAND(cname) \
    BOOST_PP_CAT(Handle, cname)({info, idx, context});
#define CLUSTER_COMMAND(cname) ClusterCommand(info, idx, context)

#define DO_POPULATE_HANDLER(name, cname, arity, type) \
  { \
    auto functor = [](const RedisCommandInfo& info, \
                      size_t idx, \
                      BatchContext* context) { \
      BOOST_PP_CAT(type, _COMMAND)(cname); \
    }; \
    yb::rpc::RpcMethodMetrics metrics(YB_REDIS_METRIC(name).Instantiate(metric_entity)); \
    setup_method({BOOST_PP_STRINGIZE(name), functor, arity, std::move(metrics)}); \
  } \
  /**/

#define POPULATE_HANDLER(z, data, elem) DO_POPULATE_HANDLER elem

class LocalCommandData {
 public:
  LocalCommandData(const RedisCommandInfo& info,
                   size_t idx,
                   BatchContext* context)
      : info_(info), idx_(idx), context_(context) {}

  const RedisClientCommand& command() const {
    return context_->command(idx_);
  }

  Slice arg(size_t i) const {
    return command()[i];
  }

  size_t arg_size() const {
    return command().size();
  }

  const std::shared_ptr<RedisInboundCall>& call() const {
    return context_->call();
  }

  client::YBClient* client() const {
    return context_->client();
  }

  const RedisServer* server() {
    return context_->server();
  }

  client::YBTable* table() const {
    return context_->table().get();
  }

  const BatchContextPtr& context() const {
    return context_;
  }

  template<class Functor>
  void Apply(const Functor& functor, const std::string& partition_key,
             ManualResponse manual_response) {
    context_->Apply(idx_, functor, partition_key, info_.metrics, manual_response);
  }

  const rpc::RpcMethodMetrics& metrics() const {
    return info_.metrics;
  }

  void Respond(const Status& status, RedisResponsePB* response) const {
    if (!status.ok()) {
      call()->RespondFailure(idx_, status);
      return;
    }

    Respond(response);
  }

  void Respond(RedisResponsePB* response = nullptr) const {
    if (response == nullptr) {
      RedisResponsePB temp;
      Respond(&temp);
      return;
    }
    const auto& cmd = command();
    VLOG_IF(4, response->has_string_response()) << "Responding to " << cmd[0].ToBuffer()
                                                << " with " << response->string_response();
    context_->call()->RespondSuccess(idx_, info_.metrics, response);
    VLOG(4) << "Done responding to " << cmd[0].ToBuffer();
  }

 private:
  const RedisCommandInfo& info_;
  size_t idx_;
  BatchContextPtr context_;
};

void GetTabletLocations(LocalCommandData data, RedisArrayPB* array_response) {
  vector<string> tablets, partitions;
  vector<master::TabletLocationsPB> locations;
  const auto table_name = RedisServiceData::GetYBTableNameForRedisDatabase(
      data.call()->connection_context().redis_db_to_use());
  auto s = data.client()->GetTabletsAndUpdateCache(
      table_name, 0, &tablets, &partitions, &locations);
  if (!s.ok()) {
    LOG(ERROR) << "Error getting tablets: " << s.message();
    return;
  }
  vector<string> response, ts_info;
  response.reserve(3);
  ts_info.reserve(2);
  for (master::TabletLocationsPB &location : locations) {
    response.clear();
    ts_info.clear();

    uint16_t start_key = 0;
    uint16_t end_key_exclusive = kRedisClusterSlots;
    if (location.partition().has_partition_key_start()) {
      if (location.partition().partition_key_start().size() == PartitionSchema::kPartitionKeySize) {
        start_key = PartitionSchema::DecodeMultiColumnHashValue(
            location.partition().partition_key_start());
      }
    }
    if (location.partition().has_partition_key_end()) {
      if (location.partition().partition_key_end().size() == PartitionSchema::kPartitionKeySize) {
        end_key_exclusive = PartitionSchema::DecodeMultiColumnHashValue(
            location.partition().partition_key_end());
      }
    }
    response.push_back(redisserver::EncodeAsInteger(start_key).ToBuffer());
    response.push_back(redisserver::EncodeAsInteger(end_key_exclusive - 1).ToBuffer());

    for (const auto &replica : location.replicas()) {
      if (replica.role() == consensus::RaftPeerPB::LEADER) {
        auto host = DesiredHostPort(replica.ts_info(), CloudInfoPB()).host();
        ts_info.push_back(redisserver::EncodeAsBulkString(host).ToBuffer());

        const auto redis_port = data.server()->opts().rpc_opts.default_port;

        VLOG(1) << "Start key: " << start_key
                << ", end key: " << end_key_exclusive - 1
                << ", node " << host
                << ", port " << redis_port;

        ts_info.push_back(redisserver::EncodeAsInteger(redis_port).ToBuffer());
        ts_info.push_back(
            redisserver::EncodeAsBulkString(replica.ts_info().permanent_uuid()).ToBuffer());
        // TODO (hector): add all the replicas to the list of redis servers in charge of this
        // partition range.
        break;
      }
    }
    response.push_back(redisserver::EncodeAsArrayOfEncodedElements(ts_info));
    array_response->add_elements(redisserver::EncodeAsArrayOfEncodedElements(response));
  }
  array_response->set_encoded(true);
}

void ClusterCommand(
    const RedisCommandInfo& info,
    size_t idx,
    BatchContext* context) {
  RedisResponsePB cluster_response;
  auto array_response = cluster_response.mutable_array_response();
  LocalCommandData data(info, idx, context);
  GetTabletLocations(data, array_response);
  context->call()->RespondSuccess(idx, info.metrics, &cluster_response);
  VLOG(1) << "Done responding to CLUSTER.";
}

void AddElements(const RefCntBuffer& buffer, RedisArrayPB* array) {
  array->add_elements(buffer.data(), buffer.size());
}

void HandleEcho(LocalCommandData data) {
  RedisResponsePB response;
  response.set_code(RedisResponsePB::OK);
  response.set_string_response(data.arg(1).ToBuffer());
  data.Respond(&response);
}

void HandleMonitor(LocalCommandData data) {
  data.Respond();

  // Add to the appenders after the call has been handled (i.e. reponded with "OK").
  auto conn = data.call()->connection().get();
  data.context()->service_data()->AppendToMonitors(conn);
}

void HandlePubSub(LocalCommandData data) {
  RedisResponsePB response;
  if (boost::iequals(data.arg(1).ToBuffer(), "CHANNELS") && data.arg_size() <= 3) {
    auto all = data.context()->service_data()->GetAllSubscriptions(AsPattern::kFalse);
    unordered_set<string> matched;
    if (data.arg_size() > 2) {
      const string& pattern = data.arg(2).ToBuffer();
      for (auto& channel : all) {
        if (RedisUtil::RedisPatternMatch(pattern, channel, /* ignore case */ false)) {
          matched.insert(channel);
        }
      }
    } else {
      matched = std::move(all);
    }

    // Build and send out an array response of all the matching channels.
    auto array_response = response.mutable_array_response();
    for (auto& channel : matched) {
      AddElements(redisserver::EncodeAsBulkString(channel), array_response);
    }
    array_response->set_encoded(true);
  } else if (boost::iequals(data.arg(1).ToBuffer(), "NUMPAT") && data.arg_size() == 2) {
    auto names = data.context()->service_data()->GetAllSubscriptions(AsPattern::kTrue);
    response.set_code(RedisResponsePB::OK);
    response.set_int_response(names.size());
  } else if (boost::iequals(data.arg(1).ToBuffer(), "NUMSUB")) {
    auto array_response = response.mutable_array_response();
    for (int idx = 2; idx < data.arg_size(); idx++) {
      const string& channel = data.arg(idx).ToBuffer();
      int subs = data.context()->service_data()->NumSubscribers(AsPattern::kFalse, channel);
      AddElements(redisserver::EncodeAsBulkString(channel), array_response);
      AddElements(redisserver::EncodeAsInteger(subs), array_response);
    }
    array_response->set_encoded(true);
  } else {
    response.set_code(RedisResponsePB_RedisStatusCode_SERVER_ERROR);
    response.set_error_message("ERR: Wrong number of arguments.");
  }
  data.Respond(&response);
}

void HandlePublish(LocalCommandData data) {
  const string& channel = data.arg(1).ToBuffer();
  const string& published_message = data.arg(2).ToBuffer();

  data.context()->service_data()->ForwardToInterestedProxies(
      channel, published_message, [data = std::move(data)](int val) {
        RedisResponsePB response;
        response.set_code(RedisResponsePB::OK);
        response.set_int_response(val);
        data.Respond(&response);
      });
}

void HandleSubscribeLikeCommand(LocalCommandData data, AsPattern as_pattern) {
  RedisResponsePB response;
  response.set_code(RedisResponsePB::OK);

  // Add to the appenders after the call has been handled (i.e. reponded with "OK").
  vector<string> channels;
  vector<int> subs;
  for (int idx = 1; idx < data.arg_size(); idx++) {
    channels.emplace_back(data.arg(idx).ToBuffer());
  }
  auto conn = data.call()->connection().get();
  data.context()->service_data()->AppendToSubscribers(as_pattern, channels, conn, &subs);
  string encoded_response;
  for (int idx = 0; idx < channels.size(); idx++) {
    encoded_response += redisserver::EncodeAsArrayOfEncodedElements(vector<string>{
        redisserver::EncodeAsBulkString(as_pattern ? "psubscribe" : "subscribe").ToBuffer(),
        redisserver::EncodeAsBulkString(channels[idx]).ToBuffer(),
        redisserver::EncodeAsInteger(subs[idx]).ToBuffer()});
  }

  VLOG(3) << "In response to [p]Subscribe queueing " << data.arg_size() - 1
          << " messages : " << encoded_response;
  response.set_encoded_response(encoded_response);
  data.Respond(&response);
}

void HandleSubscribe(LocalCommandData data) {
  HandleSubscribeLikeCommand(data, AsPattern::kFalse);
}

void HandlePSubscribe(LocalCommandData data) {
  HandleSubscribeLikeCommand(data, AsPattern::kTrue);
}

void HandleUnsubscribeLikeCommand(LocalCommandData data, AsPattern as_pattern) {
  RedisResponsePB response;
  response.set_code(RedisResponsePB::OK);

  // Add to the appenders after the call has been handled (i.e. reponded with "OK").
  auto conn = data.call()->connection().get();
  vector<string> channels;
  if (data.arg_size() > 1) {
    for (int idx = 1; idx < data.arg_size(); idx++) {
      channels.push_back(data.arg(idx).ToBuffer());
    }
  } else {
    for (auto name : data.context()->service_data()->GetSubscriptions(as_pattern, conn)) {
      channels.push_back(name);
    }
  }

  vector<int> subs;
  data.context()->service_data()->RemoveFromSubscribers(as_pattern, channels, conn, &subs);
  string encoded_response;
  for (int idx = 0; idx < channels.size(); idx++) {
    encoded_response += redisserver::EncodeAsArrayOfEncodedElements(vector<string>{
        redisserver::EncodeAsBulkString(as_pattern ? "punsubscribe" : "unsubscribe").ToBuffer(),
        redisserver::EncodeAsBulkString(channels[idx]).ToBuffer(),
        redisserver::EncodeAsInteger(subs[idx]).ToBuffer()});
  }

  VLOG(3) << "In response to [p]Unsubscribe queueing " << channels.size()
          << " messages : " << encoded_response;
  response.set_encoded_response(encoded_response);
  data.Respond(&response);
}

void HandleUnsubscribe(LocalCommandData data) {
  HandleUnsubscribeLikeCommand(data, AsPattern::kFalse);
}

void HandlePUnsubscribe(LocalCommandData data) {
  HandleUnsubscribeLikeCommand(data, AsPattern::kTrue);
}

void HandleRole(LocalCommandData data) {
  RedisResponsePB response;
  response.set_code(RedisResponsePB::OK);
  auto array_response = response.mutable_array_response();
  AddElements(redisserver::EncodeAsBulkString("master"), array_response);
  AddElements(redisserver::EncodeAsInteger(0), array_response);
  array_response->add_elements(
      redisserver::EncodeAsArrayOfEncodedElements(std::initializer_list<std::string>()));
  array_response->set_encoded(true);
  data.Respond(&response);
}

void HandleInfo(LocalCommandData data) {
  RedisResponsePB response;
  response.set_code(RedisResponsePB::OK);
  response.set_string_response(kInfoResponse);
  data.Respond(&response);
}

void HandlePing(LocalCommandData data) {
  RedisResponsePB response;
  response.set_code(RedisResponsePB::OK);
  if (data.call()->connection_context().ClientMode() == RedisClientMode::kSubscribed) {
    const auto& second = (data.arg_size() > 1 ? data.arg(1).ToBuffer() : "");
    response.set_encoded_response(redisserver::EncodeAsArrayOfEncodedElements(
        vector<string>{redisserver::EncodeAsBulkString("pong").ToBuffer(),
                       redisserver::EncodeAsBulkString(second).ToBuffer()}));
  } else {
    if (data.arg_size() > 1) {
      response.set_string_response(data.arg(1).cdata(), data.arg(1).size());
    } else {
      response.set_status_response("PONG");
    }
  }
  data.Respond(&response);
}

class RenameData : public std::enable_shared_from_this<RenameData> {
 public:
  explicit RenameData(LocalCommandData&& data) : data_(data) {
    auto table = data_.context()->table();
    const auto& source = data_.arg(1);
    const auto& dest = data_.arg(2);

    read_src_op_ = std::make_shared<client::YBRedisReadOp>(table);
    read_src_op_->mutable_request()->mutable_get_for_rename_request();
    read_src_op_->mutable_request()->mutable_key_value()->set_key(source.cdata(), source.size());

    read_ttl_op_ = std::make_shared<client::YBRedisReadOp>(table);
    read_ttl_op_->mutable_request()->mutable_get_ttl_request();
    read_ttl_op_->mutable_request()->mutable_key_value()->set_key(source.cdata(), source.size());

    delete_dest_op_ = std::make_shared<client::YBRedisWriteOp>(table);
    delete_dest_op_->mutable_request()->mutable_del_request();
    delete_dest_op_->mutable_request()->mutable_key_value()->set_key(dest.cdata(), dest.size());
    delete_dest_op_->mutable_request()->mutable_key_value()->set_type(REDIS_TYPE_NONE);

    write_dest_op_ = std::make_shared<client::YBRedisWriteOp>(table);
    write_dest_op_->mutable_request()->mutable_key_value()->set_key(dest.cdata(), dest.size());

    // write_dest_ttl_op_ will be set if needed. i.e. src has as ttl set on it.
    delete_src_op_ = std::make_shared<client::YBRedisWriteOp>(table);
    delete_src_op_->mutable_request()->mutable_del_request();
    delete_src_op_->mutable_request()->mutable_key_value()->set_key(source.cdata(), source.size());
    delete_src_op_->mutable_request()->mutable_key_value()->set_type(REDIS_TYPE_NONE);
  }

  void Execute() {
    // Rename is performed in 4 steps:
    // 1) Read from the source
    // 2) delete the destination, and overwrite the destination with contents read from the source.
    // 3) update the TTL on the destination.
    // 4) delete the source.

    std::string src_partition_key;
    auto status = read_src_op_->GetPartitionKey(&src_partition_key);
    if (!status.ok()) {
      RedisResponsePB response;
      response.set_code(RedisResponsePB_RedisStatusCode_SERVER_ERROR);
      response.set_error_message(status.message().ToBuffer());
      Respond(&response);
      return;
    }
    data_.Apply(
        std::bind(&RenameData::SaveSrcCB, shared_from_this(), _1, _2), src_partition_key,
        ManualResponse::kTrue);

    std::string dest_partition_key;
    status = write_dest_op_->GetPartitionKey(&dest_partition_key);
    if (!status.ok()) {
      RedisResponsePB response;
      response.set_code(RedisResponsePB_RedisStatusCode_SERVER_ERROR);
      response.set_error_message(status.message().ToBuffer());
      Respond(&response);
      return;
    }

    auto table = data_.context()->table();
    const std::string src_partition_start = *table->FindPartitionStart(src_partition_key);
    const std::string dest_partition_start = *table->FindPartitionStart(dest_partition_key);
    if (src_partition_start == dest_partition_start) {
      num_tablets_.store(1, std::memory_order_release);
    } else {
      data_.Apply(
          std::bind(&RenameData::SaveDestCB, shared_from_this(), _1, _2), dest_partition_key,
          ManualResponse::kTrue);
      num_tablets_.store(2, std::memory_order_release);
    }
    VLOG(1) << "num_tablets_ set to " << num_tablets_.load(std::memory_order_acquire);
  }

 private:
  LocalCommandData data_;
  std::shared_ptr<client::YBRedisReadOp> read_src_op_;
  std::shared_ptr<client::YBRedisReadOp> read_ttl_op_;
  std::shared_ptr<client::YBRedisWriteOp> delete_dest_op_;
  std::shared_ptr<client::YBRedisWriteOp> write_dest_op_;
  std::shared_ptr<client::YBRedisWriteOp> write_dest_ttl_op_;
  std::shared_ptr<client::YBRedisWriteOp> delete_src_op_;
  std::atomic_int num_tablets_{0};

  client::YBSession* session_;
  StatusFunctor src_functor_, dest_functor_;
  std::atomic<size_t> stored_{0};

  bool SaveSrcCB(client::YBSession* sess, const StatusFunctor& functor) {
    VLOG(1) << "a. SaveSrcCB";
    session_ = sess;
    src_functor_ = functor;
    if (stored_.fetch_add(1, std::memory_order_acq_rel) + 1 ==
        num_tablets_.load(std::memory_order_acquire)) {
      BeginReadSrc();
    }
    return true;
  }

  bool SaveDestCB(client::YBSession* session, const StatusFunctor& functor) {
    VLOG(1) << "b. SaveDestCB";
    dest_functor_ = functor;
    if (stored_.fetch_add(1, std::memory_order_acq_rel) + 1 ==
        num_tablets_.load(std::memory_order_acquire)) {
      BeginReadSrc();
    }
    return true;
  }

  void RespondWithError(const string& msg) {
    RedisResponsePB response;
    response.set_code(RedisResponsePB_RedisStatusCode_SERVER_ERROR);
    response.set_error_message(msg);
    Respond(&response);
  }

  void Respond(RedisResponsePB* response) {
    data_.Respond(response);
    if (src_functor_) {
      src_functor_(Status::OK());
    }
    if (dest_functor_) {
      dest_functor_(Status::OK());
    }
  }

  void BeginReadSrc() {
    VLOG(1) << "1. BeginReadSrc";
    auto table = data_.context()->table();
    if (!table) {
      RespondWithError("Table is not open");
      return;
    }

    auto status1 = session_->Apply(read_src_op_);
    auto status2 = session_->Apply(read_ttl_op_);
    if (!status1.ok() || !status2.ok()) {
      RespondWithError("Could not apply read_src_op_.");
      return;
    }
    session_->FlushAsync([retained_self = shared_from_this()](const Status& s) {
      if (!s.ok()) {
        LOG(ERROR) << "Reading from src during a Rename failed. " << s;
        retained_self->RespondWithError(s.message().ToBuffer());
      } else {
        retained_self->BeginWriteDest();
      }
    });
    VLOG(2) << "Launched read ops";
  }

  void BeginWriteDest() {
    VLOG(1) << "2. BeginWriteDest";
    RedisResponsePB readResponse = read_src_op_->response();
    if (readResponse.code() == RedisResponsePB_RedisStatusCode_NOT_FOUND) {
      // Nothing to write. The source is empty.
      RespondWithError("No such key.");
      return;
    }

    auto type = readResponse.type();
    write_dest_op_->mutable_request()->mutable_key_value()->set_type(type);
    switch (type) {
      case RedisDataType::REDIS_TYPE_STRING: {
        write_dest_op_->mutable_request()->mutable_set_request();
        write_dest_op_->mutable_request()->mutable_key_value()->add_value(
            readResponse.string_response());
        break;
      }
      case RedisDataType::REDIS_TYPE_HASH:
      case RedisDataType::REDIS_TYPE_SORTEDSET:
      case RedisDataType::REDIS_TYPE_TIMESERIES: {
        write_dest_op_->mutable_request()->mutable_set_request();
        size_t count = readResponse.array_response().elements_size();
        auto** elements = readResponse.mutable_array_response()->mutable_elements()->mutable_data();
        for (size_t i = 0; i < count; i += 2) {
          const string& first = *elements[i];
          const string& second = *elements[i + 1];
          auto req_kv = write_dest_op_->mutable_request()->mutable_key_value();
          if (type == REDIS_TYPE_SORTEDSET) {
            auto score = CheckedStold(second);
            if (!score.ok()) {
              LOG(DFATAL) << "Could not parse sorted set score " << second;
              RespondWithError("Could not parse sorted set score");
              return;
            }
            req_kv->add_subkey()->set_double_subkey(*score);
            req_kv->add_value(first);
          } else if (type == REDIS_TYPE_TIMESERIES) {
            auto ts = CheckedStoll(first);
            if (!ts.ok()) {
              LOG(DFATAL) << "Could not parse sorted set ts " << first;
              RespondWithError("Could not parse timeseries ts");
              return;
            }
            req_kv->add_subkey()->set_timestamp_subkey(*ts);
            req_kv->add_value(second);
          } else {
            req_kv->add_subkey()->set_string_subkey(first);
            req_kv->add_value(second);
          }
        }
        break;
      }
      case RedisDataType::REDIS_TYPE_SET: {
        write_dest_op_->mutable_request()->mutable_add_request();
        size_t count = readResponse.array_response().elements_size();
        auto** elements = readResponse.mutable_array_response()->mutable_elements()->mutable_data();
        for (size_t i = 0; i < count;) {
          const string& subkey = *elements[i++];
          write_dest_op_->mutable_request()->mutable_key_value()->add_subkey()->set_string_subkey(
              subkey);
        }
        break;
      }
      case RedisDataType::REDIS_TYPE_LIST:
      default: {
        LOG(DFATAL) << "Unsupported rename for type " << type;
        RespondWithError("Unsupported rename for source type");
        return;
      }
    }

    const auto& source = data_.arg(1);
    const auto& dest = data_.arg(2);
    if (source == dest) {
      // Check after ensuring that source does exist.
      // Short circuit the operation. Return success.
      RedisResponsePB response;
      Respond(&response);
      return;
    }

    RedisResponsePB ttlResponse = read_ttl_op_->response();
    int ttl_ms = ttlResponse.int_response();
    if (ttl_ms > 0) {
      auto table = data_.context()->table();
      write_dest_ttl_op_ = std::make_shared<client::YBRedisWriteOp>(table);
      write_dest_ttl_op_->mutable_request()->mutable_set_ttl_request();
      write_dest_ttl_op_->mutable_request()->mutable_key_value()->set_key(
          dest.cdata(), dest.size());
      write_dest_ttl_op_->mutable_request()->mutable_set_ttl_request()->set_ttl(ttl_ms);
    }

    auto status1 = session_->Apply(delete_dest_op_);
    auto status2 = session_->Apply(write_dest_op_);
    if (!status1.ok() || !status2.ok()) {
      RespondWithError("Could not apply deleteOps/write_dest_op_.");
      return;
    }
    session_->FlushAsync([retained_self = shared_from_this()](const Status& s) {
      if (!s.ok()) {
        LOG(ERROR) << "Writing to dest during a Rename failed. " << s;
        retained_self->RespondWithError(s.message().ToBuffer());
        return;
      }
      retained_self->BeginUpdateTTL();
    });
  }

  void BeginUpdateTTL() {
    VLOG(1) << "3. BeginUpdateTTL";
    if (!write_dest_ttl_op_) {
      BeginDeleteSrc();
      return;
    }

    auto status = session_->Apply(write_dest_ttl_op_);
    if (!status.ok()) {
      RespondWithError("Could not apply write_dest_ttl_op_.");
      return;
    }

    session_->FlushAsync([retained_self = shared_from_this()](const Status& s) {
      if (!s.ok()) {
        LOG(ERROR) << "Updating ttl for dest during a Rename failed. " << s;
        retained_self->RespondWithError(s.message().ToBuffer());
        return;
      }
      retained_self->BeginDeleteSrc();
    });
  }

  void BeginDeleteSrc() {
    VLOG(1) << "4. BeginDeleteSrc";
    auto status = session_->Apply(delete_src_op_);
    if (!status.ok()) {
      RespondWithError("Could not apply delete_src_op_.");
      return;
    }
    session_->FlushAsync([retained_self = shared_from_this()](const Status& s) {
      if (!s.ok()) {
        LOG(ERROR) << "Deleting src during a Rename failed. " << s;
        retained_self->RespondWithError(s.message().ToBuffer());
        return;
      }
      RedisResponsePB response;
      retained_self->Respond(&response);
    });
  }
};

void HandleRename(LocalCommandData data) {
  VLOG(1) << "0. HandleRename";
  std::shared_ptr<RenameData> rename_data = std::make_shared<RenameData>(std::move(data));
  rename_data->Execute();
}

class KeysProcessor : public std::enable_shared_from_this<KeysProcessor> {
 public:
  explicit KeysProcessor(const LocalCommandData& data)
      : data_(data),
        partitions_(data.table()->GetPartitionsCopy()), sessions_(partitions_.size()),
        callbacks_(partitions_.size()) {
    resp_.set_code(RedisResponsePB::OK);
  }

  bool Store(size_t idx, client::YBSession* session, const StatusFunctor& callback) {
    sessions_[idx] = session;
    callbacks_[idx] = callback;
    if (stored_.fetch_add(1, std::memory_order_acq_rel) + 1 == callbacks_.size()) {
      Execute(0);
    }
    return true;
  }

  const std::vector<std::string>& partitions() const {
    return partitions_;
  }

 private:
  void Execute(size_t idx) {
    if (idx == partitions_.size()) {
      ProcessedAll(Status::OK());
      return;
    }

    const auto& partition_key = partitions_[idx];
    auto operation = std::make_shared<client::YBRedisReadOp>(data_.table()->shared_from_this());
    auto request = operation->mutable_request();
    uint16_t hash_code = partition_key.size() == 0 ?
        0 : PartitionSchema::DecodeMultiColumnHashValue(partition_key);
    request->mutable_key_value()->set_hash_code(hash_code);
    request->mutable_keys_request()->set_pattern(data_.arg(1).ToBuffer());
    request->mutable_keys_request()->set_threshold(keys_threshold_);
    sessions_[idx]->set_allow_local_calls_in_curr_thread(false);
    auto status = sessions_[idx]->Apply(operation);
    if (!status.ok()) {
      ProcessedAll(status);
      return;
    }
    sessions_[idx]->FlushAsync(std::bind(
        &KeysProcessor::ProcessedOne, shared_from_this(), idx, operation, _1));
  }

  void ProcessedOne(
      size_t idx, const std::shared_ptr<client::YBRedisReadOp>& operation, const Status& status) {
    if (!status.ok()) {
      ProcessedAll(status);
      return;
    }

    auto& response = *operation->mutable_response();
    if (response.code() == RedisResponsePB::SERVER_ERROR) {
      // We received too many keys or timed out, forwarding the error message.
      resp_ = response;
      ProcessedAll(Status::OK());
      return;
    }

    size_t count = response.array_response().elements_size();
    auto** elements = response.mutable_array_response()->mutable_elements()->mutable_data();
    keys_threshold_ -= count;

    auto& array_response = *resp_.mutable_array_response();
    for (size_t i = 0; i != count; ++i) {
      array_response.mutable_elements()->AddAllocated(elements[i]);
    }

    response.mutable_array_response()->mutable_elements()->ExtractSubrange(0, count, nullptr);

    if (keys_threshold_ == 0) {
      ProcessedAll(Status::OK());
      return;
    }

    Execute(idx + 1);
  }

  void ProcessedAll(const Status& status) {
    data_.Respond(status, &resp_);

    for (const auto& callback : callbacks_) {
      callback(status);
    }
  }

  LocalCommandData data_;

  std::vector<std::string> partitions_;
  std::vector<client::YBSession*> sessions_;
  std::vector<StatusFunctor> callbacks_;
  std::atomic<size_t> stored_{0};
  RedisResponsePB resp_;
  size_t keys_threshold_ = FLAGS_redis_keys_threshold;
};

void HandleKeys(LocalCommandData data) {
  auto processor = std::make_shared<KeysProcessor>(data);
  size_t idx = 0;
  for (const std::string& partition_key : processor->partitions()) {
    data.Apply(std::bind(
        &KeysProcessor::Store, processor, idx, _1, _2), partition_key, ManualResponse::kTrue);
    ++idx;
  }
}

void HandleCommand(LocalCommandData data) {
  data.Respond();
}

void HandleQuit(LocalCommandData data) {
  data.call()->MarkForClose();
  data.Respond();
}

bool AcceptPassword(const vector<string>& allowed, const string& candidate) {
  for (auto& stored_hash_or_pwd : allowed) {
    if (FLAGS_use_hashed_redis_password
            ? (0 == yb::util::bcrypt_checkpw(candidate.c_str(), stored_hash_or_pwd.c_str()))
            : (stored_hash_or_pwd == candidate)) {
      return true;
    }
  }
  return false;
}

void HandleConfig(LocalCommandData data) {
  RedisResponsePB resp;
  // We only handle config requests of the type:
  // CONFIG SET REQUIREPASS <password>
  // everything else is handled as a no-op.
  if (data.arg_size() != 4 ||
      !(boost::iequals(data.arg(1).ToBuffer(), "SET") &&
        boost::iequals(data.arg(2).ToBuffer(), "REQUIREPASS"))) {
    if (data.arg_size() >= 2 && boost::iequals(data.arg(1).ToBuffer(), "GET")) {
      // CONFIG GET will be responded to with an empty array.
      resp.mutable_array_response()->set_encoded(false);
    }
    data.Respond(&resp);
    return;
  }

  // Handle Config Set Requirepass <passwords>
  DCHECK_EQ(FLAGS_redis_passwords_separator.size(), 1);
  vector<string> passwords =
      yb::StringSplit(data.arg(3).ToBuffer(), FLAGS_redis_passwords_separator[0]);
  Status status;
  if (passwords.size() > 2) {
    status = STATUS(InvalidArgument, "Only maximum of 2 passwords are supported");
  } else if (FLAGS_use_hashed_redis_password) {
    std::vector<string> hashes;
    for (const auto& pwd : passwords) {
      char hash[yb::util::kBcryptHashSize];
      if (yb::util::bcrypt_hashpw(pwd.c_str(), hash) != 0) {
        resp.set_code(RedisResponsePB_RedisStatusCode_SERVER_ERROR);
        resp.set_error_message("ERR: Error while hashing the password.");
        data.Respond(&resp);
        return;
      }
      hashes.emplace_back(hash, yb::util::kBcryptHashSize);
    }
    status = data.client()->SetRedisPasswords(hashes);
  } else {
    status = data.client()->SetRedisPasswords(passwords);
  }

  if (!status.ok()) {
    resp.set_code(RedisResponsePB_RedisStatusCode_SERVER_ERROR);
    resp.set_error_message(StrCat("ERR: ", status.ToString()));
  } else {
    resp.set_code(RedisResponsePB::OK);
  }
  data.Respond(&resp);
}

void HandleAuth(LocalCommandData data) {
  vector<string> passwords;
  auto status = data.context()->service_data()->GetRedisPasswords(&passwords);
  RedisResponsePB resp;
  if (!status.ok() || !AcceptPassword(passwords, data.arg(1).ToBuffer())) {
    resp.set_code(RedisResponsePB_RedisStatusCode_SERVER_ERROR);
    auto error_message =
        (status.ok() ? "ERR: Bad Password."
                     : strings::Substitute("ERR: Bad Password. $0", status.ToString()));
    resp.set_error_message(error_message);
  } else {
    RedisConnectionContext& context = data.call()->connection_context();
    context.set_authenticated(true);
    resp.set_code(RedisResponsePB::OK);
  }
  data.Respond(&resp);
}

void FlushDBs(LocalCommandData data, const vector<string> ids) {
  RedisResponsePB resp;

  const Status s = FLAGS_yedis_enable_flush
                       ? data.client()->TruncateTables(ids)
                       : STATUS(InvalidArgument, "FLUSHDB and FLUSHALL are not enabled.");

  if (s.ok()) {
    resp.set_code(RedisResponsePB_RedisStatusCode_OK);
  } else {
    const Slice message = s.message();
    resp.set_code(RedisResponsePB_RedisStatusCode_SERVER_ERROR);
    resp.set_error_message(message.data(), message.size());
  }
  data.Respond(&resp);
}

void HandleFlushDB(LocalCommandData data) {
  FlushDBs(data, {data.table()->id()});
}

void HandleFlushAll(LocalCommandData data) {
  const string prefix = common::kRedisTableName;
  auto result = data.client()->ListTables(prefix);
  if (!result.ok()) {
    RedisResponsePB resp;
    const Slice message = result.status().message();
    resp.set_code(RedisResponsePB_RedisStatusCode_SERVER_ERROR);
    resp.set_error_message(message.data(), message.size());
    data.Respond(&resp);
    return;
  }
  const auto& table_names = *result;
  // Gather table ids.
  vector<string> table_ids;
  for (const auto& name : table_names) {
    std::shared_ptr<client::YBTable> table;
    const auto s = data.client()->OpenTable(name, &table);
    if (!s.ok()) {
      RedisResponsePB resp;
      const Slice message = s.message();
      resp.set_code(RedisResponsePB_RedisStatusCode_SERVER_ERROR);
      resp.set_error_message(message.data(), message.size());
      data.Respond(&resp);
      return;
    }
    table_ids.push_back(table->id());
  }
  FlushDBs(data, table_ids);
}

void HandleCreateDB(LocalCommandData data) {
  RedisResponsePB resp;
  // Ensure that the rediskeyspace exists. If not create it.
  Status s = data.client()->CreateNamespaceIfNotExists(common::kRedisKeyspaceName,
                                                       YQLDatabase::YQL_DATABASE_REDIS);
  if (!s.ok()) {
    VLOG(1) << "Namespace '" << common::kRedisKeyspaceName << "' could not be created.";
    const Slice message = s.message();
    resp.set_code(RedisResponsePB_RedisStatusCode_SERVER_ERROR);
    resp.set_error_message(message.data(), message.size());
    data.Respond(&resp);
    return;
  }

  // Figure out the redis table name that we should be using.
  const string db_name = data.arg(1).ToBuffer();
  const auto table_name = RedisServiceData::GetYBTableNameForRedisDatabase(db_name);
  std::unique_ptr<yb::client::YBTableCreator> table_creator(data.client()->NewTableCreator());
  s = table_creator->table_name(table_name)
          .table_type(yb::client::YBTableType::REDIS_TABLE_TYPE)
          .Create();
  if (s.ok()) {
    resp.set_code(RedisResponsePB_RedisStatusCode_OK);
  } else if (s.IsAlreadyPresent()) {
    VLOG(1) << "Table '" << table_name.ToString() << "' already exists";
    resp.set_code(RedisResponsePB_RedisStatusCode_OK);
  } else {
    const Slice message = s.message();
    resp.set_code(RedisResponsePB_RedisStatusCode_SERVER_ERROR);
    resp.set_error_message(message.data(), message.size());
  }
  data.Respond(&resp);
}

void HandleListDB(LocalCommandData data) {
  RedisResponsePB resp;
  // Figure out the redis table name that we should be using.
  const string prefix = common::kRedisTableName;
  const size_t prefix_len = strlen(common::kRedisTableName);
  const auto result = data.client()->ListTables(prefix);
  if (!result.ok()) {
    const Slice message = result.status().message();
    resp.set_code(RedisResponsePB_RedisStatusCode_SERVER_ERROR);
    resp.set_error_message(message.data(), message.size());
    data.Respond(&resp);
    return;
  }
  const auto& table_names = *result;
  auto array_response = resp.mutable_array_response();
  vector<string> dbs;
  for (const auto& ybname : table_names) {
    if (!ybname.is_redis_table()) continue;
    const auto& tablename = ybname.table_name();
    if (tablename == common::kRedisTableName) {
      dbs.push_back("0");
    } else {
      // Of the form <prefix>_<DB>.
      dbs.push_back(tablename.substr(prefix_len + 1));
    }
  }
  std::sort(dbs.begin(), dbs.end());
  for (const string& db : dbs) {
    AddElements(redisserver::EncodeAsBulkString(db), array_response);
  }
  array_response->set_encoded(true);
  resp.set_code(RedisResponsePB::OK);
  data.Respond(&resp);
}

void HandleDeleteDB(LocalCommandData data) {
  RedisResponsePB resp;
  // Figure out the redis table name that we should be using.
  const string db_name = data.arg(1).ToBuffer();
  const auto table_name = RedisServiceData::GetYBTableNameForRedisDatabase(db_name);

  Status s = data.client()->DeleteTable(table_name, /* wait */ true);
  if (s.ok()) {
    resp.set_code(RedisResponsePB_RedisStatusCode_OK);
  } else if (s.IsNotFound()) {
    VLOG(1) << "Table '" << table_name.ToString() << "' does not exist.";
    resp.set_code(RedisResponsePB_RedisStatusCode_OK);
  } else {
    const Slice message = s.message();
    resp.set_code(RedisResponsePB_RedisStatusCode_SERVER_ERROR);
    resp.set_error_message(message.data(), message.size());
  }
  data.Respond(&resp);
}

void HandleSelect(LocalCommandData data) {
  RedisResponsePB resp;
  const string db_name = data.arg(1).ToBuffer();
  RedisServiceData* sd = data.context()->service_data();
  auto s = sd->GetYBTableForDB(db_name);
  if (s.ok()) {
    // Update RedisConnectionContext to use the specified table.
    RedisConnectionContext& context = data.call()->connection_context();
    context.use_redis_db(db_name);
    resp.set_code(RedisResponsePB_RedisStatusCode_OK);
  } else {
    const Slice message = s.status().message();
    VLOG(1) << " Could not open Redis Table for db " << db_name << " : " << message.ToString();
    resp.set_code(RedisResponsePB_RedisStatusCode_SERVER_ERROR);
    resp.set_error_message(message.data(), message.size());
    data.call()->MarkForClose();
  }
  data.Respond(&resp);
}

void HandleDebugSleep(LocalCommandData data) {
  struct SleepWaiter {
    std::chrono::steady_clock::time_point end;
    StatusFunctor callback;
    LocalCommandData data;

    void operator()(const Status& status) const {
      if (!status.ok()) {
        callback(status);
        return;
      }
      if (data.call()->aborted()) {
        callback(STATUS(Aborted, ""));
        return;
      }
      auto now = std::chrono::steady_clock::now();
      if (now >= end) {
        callback(Status::OK());
        return;
      }
      data.context()->client()->messenger()->scheduler().Schedule(
          *this, std::min(end, now + 100ms));
    }
  };

  auto time_ms = CheckedStoll(data.arg(1));
  if (!time_ms.ok()) {
    RedisResponsePB resp;
    resp.set_code(RedisResponsePB::PARSING_ERROR);
    const Slice message = time_ms.status().message();
    resp.set_error_message(message.data(), message.size());
    data.Respond(&resp);
  }

  auto now = std::chrono::steady_clock::now();
  auto functor = [end = now + std::chrono::milliseconds(*time_ms),
                  data](client::YBSession*, const StatusFunctor& callback) {
    SleepWaiter waiter{ end, callback, data };
    waiter(Status::OK());
    return true;
  };

  data.Apply(functor, std::string(), ManualResponse::kFalse);
}

} // namespace

void RespondWithFailure(
    std::shared_ptr<RedisInboundCall> call,
    size_t idx,
    const std::string& error,
    const char* redis_code) {
  // process the request
  DVLOG(4) << " Processing request from client ";
  const auto& command = call->client_batch()[idx];
  size_t size = command.size();
  for (size_t i = 0; i < size; i++) {
    DVLOG(4) << i + 1 << " / " << size << " : " << command[i].ToDebugString(8);
  }

  // Send the result.
  DVLOG(4) << "Responding to call " << call->ToString() << " with failure " << error;
  std::string cmd = command[0].ToBuffer();
  call->RespondFailure(idx, STATUS_FORMAT(InvalidCommand, "$0 $1: $2", redis_code, cmd, error));
}

void FillRedisCommands(const scoped_refptr<MetricEntity>& metric_entity,
                       const std::function<void(const RedisCommandInfo& info)>& setup_method) {
  BOOST_PP_SEQ_FOR_EACH(POPULATE_HANDLER, ~, REDIS_COMMANDS);
}

} // namespace redisserver
} // namespace yb
