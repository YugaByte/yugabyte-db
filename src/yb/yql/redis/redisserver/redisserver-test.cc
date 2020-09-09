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

#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#include <boost/algorithm/string.hpp>

#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/client/meta_cache.h"

#include "yb/integration-tests/redis_table_test_base.h"

#include "yb/yql/redis/redisserver/redis_client.h"
#include "yb/yql/redis/redisserver/redis_constants.h"
#include "yb/yql/redis/redisserver/redis_encoding.h"
#include "yb/yql/redis/redisserver/redis_server.h"

#include "yb/rpc/io_thread_pool.h"

#include "yb/util/cast.h"
#include "yb/util/enums.h"
#include "yb/util/protobuf.h"
#include "yb/util/test_util.h"
#include "yb/util/value_changer.h"

#include "yb/master/flush_manager.h"

DECLARE_uint64(redis_max_concurrent_commands);
DECLARE_uint64(redis_max_batch);
DECLARE_uint64(redis_max_read_buffer_size);
DECLARE_uint64(redis_max_queued_bytes);
DECLARE_int64(redis_rpc_block_size);
DECLARE_bool(redis_safe_batch);
DECLARE_bool(emulate_redis_responses);
DECLARE_bool(TEST_tserver_timeout);
DECLARE_bool(TEST_enable_backpressure_mode_for_testing);
DECLARE_bool(yedis_enable_flush);
DECLARE_int32(redis_service_yb_client_timeout_millis);
DECLARE_int32(redis_max_value_size);
DECLARE_int32(redis_max_command_size);
DECLARE_int32(redis_password_caching_duration_ms);
DECLARE_int32(rpc_max_message_size);
DECLARE_int32(consensus_max_batch_size_bytes);
DECLARE_int32(consensus_rpc_timeout_ms);
DECLARE_int64(max_time_in_queue_ms);

DEFINE_uint64(test_redis_max_concurrent_commands, 20,
    "Value of redis_max_concurrent_commands for pipeline test");
DEFINE_uint64(test_redis_max_batch, 250,
    "Value of redis_max_batch for pipeline test");

METRIC_DECLARE_gauge_uint64(redis_available_sessions);
METRIC_DECLARE_gauge_uint64(redis_allocated_sessions);
METRIC_DECLARE_gauge_uint64(redis_monitoring_clients);

using namespace std::literals;
using namespace std::placeholders;

namespace yb {
namespace redisserver {

using std::string;
using std::unique_ptr;
using std::vector;
using strings::Substitute;
using yb::integration_tests::RedisTableTestBase;
using yb::util::ToRepeatedPtrField;

#if defined(ADDRESS_SANITIZER) || defined(THREAD_SANITIZER)
constexpr int kDefaultTimeoutMs = 100000;
#else
constexpr int kDefaultTimeoutMs = 10000;
#endif

typedef std::tuple<string, string> CollectionEntry;

class TestRedisService : public RedisTableTestBase {
 public:
  void SetUp() override;
  void TearDown() override;

  void StartServer();
  void StopServer();
  void StartClient();
  void StopClient();
  void RestartClient();
  void SendCommandAndExpectTimeout(const string& cmd);

  void SendCommandAndExpectResponse(int line,
      const string& cmd,
      const string& resp,
      bool partial = false);

  void SendCommandAndExpectResponse(int line,
      const RefCntBuffer& cmd,
      const RefCntBuffer& resp,
      bool partial = false) {
    SendCommandAndExpectResponse(line, cmd.ToBuffer(), resp.ToBuffer(), partial);
  }

  template <class Callback>
  void DoRedisTest(int line,
      const std::vector<std::string>& command,
      RedisReplyType reply_type,
      const Callback& callback);

  void DoRedisTestString(int line,
      const std::vector<std::string>& command,
      const std::string& expected,
      RedisReplyType type = RedisReplyType::kStatus) {
    DoRedisTest(line, command, type,
        [line, expected](const RedisReply& reply) {
          ASSERT_EQ(expected, reply.as_string()) << "Originator: " << __FILE__ << ":" << line;
        }
    );
  }

  void DoRedisTestSimpleString(int line,
      const std::vector<std::string>& command,
      const std::string& expected) {
    DoRedisTestString(line, command, expected, RedisReplyType::kStatus);
  }

  void DoRedisTestBulkString(int line,
      const std::vector<std::string>& command,
      const std::string& expected) {
    DoRedisTestString(line, command, expected, RedisReplyType::kString);
  }

  void DoRedisTestOk(int line, const std::vector<std::string>& command) {
    DoRedisTestSimpleString(line, command, "OK");
  }

  void DoRedisTestExpectError(int line, const std::vector<std::string>& command,
                              const std::string& error_prefix = "") {
    DoRedisTest(line, command, RedisReplyType::kError,
        [line, error_prefix](const RedisReply& reply) {
          if (!error_prefix.empty()) {
            ASSERT_EQ(reply.error().find(error_prefix), 0) << "Error message has the wrong prefix "
              << " expected : " << error_prefix << " got message " << reply.error()
              << " Originator: " << __FILE__ << ":" << line;
          }
        }
    );
  }

  void DoRedisTestExpectSimpleStringEndingWith(
      int line, const std::vector<std::string>& command, const std::string& suffix = "") {
    DoRedisTest(line, command, RedisReplyType::kStatus, [line, suffix](const RedisReply& reply) {
      if (!suffix.empty()) {
        ASSERT_TRUE(boost::algorithm::ends_with(reply.as_string(), suffix))
            << "Reply has the wrong suffix. Expected '" << reply.as_string() << "' to end in "
            << suffix << " Originator: " << __FILE__ << ":" << line;
      }
    });
  }

  void DoRedisTestExpectErrorMsg(int line, const std::vector<std::string>& command,
      const std::string& error_msg) {
    DoRedisTestString(line, command, error_msg, RedisReplyType::kError);
  }

  void DoRedisTestInt(int line,
                      const std::vector<std::string>& command,
                      int64_t expected) {
    DoRedisTest(line, command, RedisReplyType::kInteger,
        [line, expected](const RedisReply& reply) {
          ASSERT_EQ(expected, reply.as_integer()) << "Originator: " << __FILE__ << ":" << line;
        }
    );
  }

  void DoRedisTestIntRange(
      int line,
      const std::vector<std::string>& command,
      int64_t expected_min, int64_t expected_max) {
    DoRedisTest(line, command, RedisReplyType::kInteger,
        [line, expected_min, expected_max](const RedisReply& reply) {
          ASSERT_LE(expected_min, reply.as_integer()) << "Originator: " << __FILE__ << ":" << line;
          ASSERT_GE(expected_max, reply.as_integer()) << "Originator: " << __FILE__ << ":" << line;
        }
    );
  }

  void DoRedisTestApproxInt(int line,
                            const std::vector<std::string>& command,
                            int64_t expected,
                            int64_t err_bound) {
    DoRedisTestIntRange(line, command, expected - err_bound, expected + err_bound);
  }

  void DoRedisTestResultsArray(
      int line, const std::vector<std::string>& command, const std::vector<RedisReply>& expected) {
    DoRedisTest(line, command, RedisReplyType::kArray,
        [line, expected](const RedisReply& reply) {
          const auto& replies = reply.as_array();
          ASSERT_EQ(expected.size(), replies.size())
              << "Originator: " << __FILE__ << ":" << line << std::endl
              << "Expected: " << yb::ToString(expected) << std::endl
              << " Replies: " << reply.ToString();
          for (size_t i = 0; i < expected.size(); i++) {
            DVLOG(3) << "Checking " << replies[i].ToString();
            if (expected[i].get_type() == RedisReplyType::kString &&
                expected[i].as_string() == "IGNORED") {
              continue;
            }
            ASSERT_EQ(expected[i], replies[i])
                << "Originator: " << __FILE__ << ":" << line << ", i: " << i << " expected[i] "
                << yb::ToString(expected[i]) << " replies[i] " << yb::ToString(replies[i]);
          }
        }
    );
  }

  // Note: expected empty string will check for null instead.
  void DoRedisTestArray(
      int line, const std::vector<std::string>& command, const std::vector<std::string>& expected) {
    std::vector<RedisReply> redis_replies;
    for (size_t i = 0; i < expected.size(); i++) {
      redis_replies.push_back(RedisReply(RedisReplyType::kString, expected[i]));
    }
    DoRedisTestResultsArray(line, command, redis_replies);
  }

  void DoRedisTestDouble(int line, const std::vector<std::string>& command, double expected) {
    DoRedisTest(line, command, RedisReplyType::kString,
                [line, expected](const RedisReply& reply) {
                  std::string::size_type sz;
                  double reply_score = std::stod(reply.as_string(), &sz);
                  ASSERT_EQ(reply_score, expected) << "Originator: " << __FILE__ << ":" << line;
                }
    );
  }

  // Used to check pairs of doubles and strings, for range scans withscores.
  void DoRedisTestScoreValueArray(int line,
      const std::vector<std::string>& command,
      const std::vector<double>& expected_scores,
      const std::vector<std::string>& expected_values) {
    ASSERT_EQ(expected_scores.size(), expected_values.size());
    DoRedisTest(line, command, RedisReplyType::kArray,
      [line, expected_scores, expected_values](const RedisReply& reply) {
        const auto& replies = reply.as_array();
        ASSERT_EQ(expected_scores.size() * 2, replies.size())
                      << "Originator: " << __FILE__ << ":" << line;
        for (size_t i = 0; i < expected_scores.size(); i++) {
          ASSERT_EQ(expected_values[i], replies[2 * i].as_string())
                        << "Originator: " << __FILE__ << ":" << line << ", i: " << i;
          std::string::size_type sz;
          double reply_score = std::stod(replies[2 * i + 1].as_string(), &sz);
          ASSERT_EQ(expected_scores[i], reply_score)
                        << "Originator: " << __FILE__ << ":" << line << ", i: " << i;
        }
      }
    );
  }

  void DoRedisTestNull(int line,
      const std::vector<std::string>& command) {
    DoRedisTest(line, command, RedisReplyType::kNull,
        [line](const RedisReply& reply) {
          ASSERT_TRUE(reply.is_null()) << "Originator: " << __FILE__ << ":" << line;
        }
    );
  }

  inline void CheckExpired(std::string* key) {
    SyncClient();
    DoRedisTestInt(__LINE__, {"TTL", *key}, -2);
    DoRedisTestInt(__LINE__, {"PTTL", *key}, -2);
    DoRedisTestInt(__LINE__, {"EXPIRE", *key, "5"}, 0);
    SyncClient();
  }

  inline void CheckExpiredPrimitive(std::string* key) {
    SyncClient();
    DoRedisTestNull(__LINE__, {"GET", *key});
    CheckExpired(key);
  }

  void SyncClient() { client().Commit(); }

  void VerifyCallbacks();

  Status FlushRedisTable() {
    // Flush the table
    master::FlushTablesRequestPB req;
    req.set_is_compaction(false);
    table_name().SetIntoTableIdentifierPB(req.add_tables());
    master::FlushTablesResponsePB resp;
    RETURN_NOT_OK(mini_cluster()->leader_mini_master()->master()->flush_manager()->
                  FlushTables(&req, &resp));

    master::IsFlushTablesDoneRequestPB wait_req;
    // Wait for table creation.
    wait_req.set_flush_request_id(resp.flush_request_id());

    for (int k = 0; k < 20; ++k) {
      master::IsFlushTablesDoneResponsePB wait_resp;
      RETURN_NOT_OK(mini_cluster()->leader_mini_master()->master()->flush_manager()->
                    IsFlushTablesDone(&wait_req, &wait_resp));
      if (wait_resp.done()) {
        return Status::OK();
      }
      SleepFor(MonoDelta::FromSeconds(1));
    }
    return STATUS(IllegalState, "Could not flush redis table.");
  }

  int server_port() { return redis_server_port_; }

  CHECKED_STATUS Send(const std::string& cmd);

  CHECKED_STATUS SendCommandAndGetResponse(
      const string& cmd, int expected_resp_length, int timeout_in_millis = kDefaultTimeoutMs);

  size_t CountSessions(const GaugePrototype<uint64_t>& proto) {
    constexpr uint64_t kInitialValue = 0UL;
    auto counter = server_->metric_entity()->FindOrCreateGauge(&proto, kInitialValue);
    return counter->value();
  }

  virtual Endpoint RedisProxyEndpoint() {
    return Endpoint(IpAddress(), server_port());
  }

  void TestTSTtl(const std::string& expire_command, int64_t ttl_sec, int64_t expire_val,
      const std::string& redis_key) {
    DoRedisTestOk(__LINE__, {"TSADD", redis_key, "10", "v1", "20", "v2", "30", "v3", expire_command,
        strings::Substitute("$0", expire_val)});
    SyncClient();
    DoRedisTestOk(__LINE__, {"TSADD", redis_key, "40", "v4"});
    DoRedisTestOk(__LINE__, {"TSADD", redis_key, "10", "v5"});
    DoRedisTestOk(__LINE__, {"TSADD", redis_key, "50", "v6", expire_command,
        std::to_string(expire_val + ttl_sec)});
    DoRedisTestOk(__LINE__, {"TSADD", redis_key, "60", "v7", expire_command,
        std::to_string(expire_val - ttl_sec + kRedisMaxTtlSeconds)});
    DoRedisTestOk(__LINE__, {"TSADD", redis_key, "70", "v8", expire_command,
        std::to_string(expire_val - ttl_sec + kRedisMinTtlSetExSeconds)});
    // Same kv with different ttl (later one should win).
    DoRedisTestOk(__LINE__, {"TSADD", redis_key, "80", "v9", expire_command,
        std::to_string(expire_val)});
    DoRedisTestOk(__LINE__, {"TSADD", redis_key, "80", "v9", expire_command,
        std::to_string(expire_val + ttl_sec)});
    SyncClient();

    // Wait for min ttl to expire.
    std::this_thread::sleep_for(std::chrono::seconds(kRedisMinTtlSetExSeconds + 1));

    SyncClient();
    DoRedisTestBulkString(__LINE__, {"TSGET", redis_key, "10"}, "v5");
    DoRedisTestBulkString(__LINE__, {"TSGET", redis_key, "20"}, "v2");
    DoRedisTestBulkString(__LINE__, {"TSGET", redis_key, "30"}, "v3");
    DoRedisTestBulkString(__LINE__, {"TSGET", redis_key, "50"}, "v6");
    DoRedisTestBulkString(__LINE__, {"TSGET", redis_key, "60"}, "v7");
    DoRedisTestBulkString(__LINE__, {"TSGET", redis_key, "80"}, "v9");
    SyncClient();

    // Wait for TTL expiry
    std::this_thread::sleep_for(std::chrono::seconds(ttl_sec + 1));
    DoRedisTestBulkString(__LINE__, {"TSGET", redis_key, "10"}, "v5");
    DoRedisTestBulkString(__LINE__, {"TSGET", redis_key, "40"}, "v4");
    DoRedisTestBulkString(__LINE__, {"TSGET", redis_key, "50"}, "v6");
    DoRedisTestBulkString(__LINE__, {"TSGET", redis_key, "60"}, "v7");
    DoRedisTestBulkString(__LINE__, {"TSGET", redis_key, "80"}, "v9");
    DoRedisTestNull(__LINE__, {"TSGET", redis_key, "20"});
    DoRedisTestNull(__LINE__, {"TSGET", redis_key, "30"});
    SyncClient();

    // Wait for next TTL expiry
    std::this_thread::sleep_for(std::chrono::seconds(ttl_sec + 1));
    DoRedisTestBulkString(__LINE__, {"TSGET", redis_key, "10"}, "v5");
    DoRedisTestBulkString(__LINE__, {"TSGET", redis_key, "40"}, "v4");
    DoRedisTestNull(__LINE__, {"TSGET", redis_key, "20"});
    DoRedisTestNull(__LINE__, {"TSGET", redis_key, "30"});
    DoRedisTestNull(__LINE__, {"TSGET", redis_key, "50"});
    DoRedisTestBulkString(__LINE__, {"TSGET", redis_key, "60"}, "v7");
    SyncClient();
    VerifyCallbacks();

    // Test invalid commands.
    DoRedisTestExpectError(__LINE__, {"TSADD", redis_key, "10", "v1", expire_command,
        std::to_string(expire_val - 2 * ttl_sec)}); // Negative ttl.
    DoRedisTestExpectError(__LINE__, {"TSADD", redis_key, "10", "v1", "20", "v2", "30", "v3",
        expire_command});
    DoRedisTestExpectError(__LINE__, {"TSADD", redis_key, "10", "v1", expire_command, "v2", "30",
        "v3"});
    DoRedisTestExpectError(__LINE__, {"TSADD", expire_command, redis_key, "10", "v1", "30", "v3"});
    DoRedisTestExpectError(__LINE__, {"TSADD", redis_key, "10", "v1", expire_command, "abc"});
    DoRedisTestExpectError(__LINE__, {"TSADD", redis_key, "10", "v1", expire_command, "3.0"});
    DoRedisTestExpectError(__LINE__, {"TSADD", redis_key, "10", "v1", expire_command, "123 "});
    DoRedisTestExpectError(__LINE__, {"TSADD", redis_key, "10", "v1", expire_command,
        "9223372036854775808"});
    DoRedisTestExpectError(__LINE__, {"TSADD", redis_key, "10", "v1", expire_command,
        "-9223372036854775809"});
    DoRedisTestExpectError(__LINE__, {"TSADD", redis_key, "10", "v1", expire_command,
        std::to_string(expire_val - ttl_sec)}); // ttl of 0 not allowed.
  }

  void TestFlush(const string& flush_cmd, const bool allow_flush) {
    FLAGS_yedis_enable_flush = allow_flush;
    // Populate keys.
    const int kKeyCount = 100;
    for (int i = 0; i < kKeyCount; i++) {
      DoRedisTestOk(__LINE__, {"SET", Substitute("k$0", i),  Substitute("v$0", i)});
    }
    SyncClient();

    // Verify keys.
    for (int i = 0; i < kKeyCount; i++) {
      DoRedisTestBulkString(__LINE__, {"GET", Substitute("k$0", i)}, Substitute("v$0", i));
    }
    SyncClient();

    if (!allow_flush) {
      DoRedisTestExpectError(__LINE__, {flush_cmd});
      SyncClient();
      return;
    }

    // Delete all keys in the database and verify keys are gone.
    DoRedisTestOk(__LINE__, {flush_cmd});
    SyncClient();
    for (int i = 0; i < kKeyCount; i++) {
      DoRedisTestNull(__LINE__, {"GET", Substitute("k$0", i)});
    }
    SyncClient();

    // Delete all keys in the database again (an NOOP) and verify there is no issue.
    DoRedisTestOk(__LINE__, {flush_cmd});
    SyncClient();
    for (int i = 0; i < kKeyCount; i++) {
      DoRedisTestNull(__LINE__, {"GET", Substitute("k$0", i)});
    }
    SyncClient();
  }

  bool expected_no_sessions_ = false;

  RedisClient& client() {
    if (!test_client_) {
      io_thread_pool_.emplace("test", 1);
      auto endpoint = RedisProxyEndpoint();
      test_client_ = std::make_shared<RedisClient>(endpoint.address().to_string(), endpoint.port());
    }
    return *test_client_;
  }

  void UseClient(std::shared_ptr<RedisClient> client) {
    VLOG(1) << "Using " << client.get() << " replacing " << test_client_.get();
    test_client_ = client;
  }

  void CloseRedisClient();

  // Tests not repeated because they are already covered in the primitive TTL test:
  // Operating on a key that does not exist, EXPIRing with a TTL out of bounds,
  // Any (P) version of a command.
  template <typename T>
  void TestTtlCollection(std::string* collection_key, T* values, int val_size,
                         std::function<void(std::string*, T*, int)> set_vals,
                         std::function<void(std::string*, T*)> add_elems,
                         std::function<void(std::string*, T*)> del_elems,
                         std::function<void(std::string*, T*, bool)> get_check,
                         std::function<void(std::string*, int)> check_card) {

    // num_shifts is the number of times we call modify
    int num_shifts = 7;
    int size = val_size - num_shifts;
    std::string key = *collection_key;
    auto init = std::bind(set_vals, collection_key, std::placeholders::_1, size);
    auto modify = [add_elems, del_elems, check_card, collection_key, size, this]
                  (T** values) {
                    SyncClient();
                    if (std::rand() % 2) {
                      del_elems(collection_key, *values);
                      SyncClient();
                      check_card(collection_key, size - 1);
                      SyncClient();
                      add_elems(collection_key, *values + size);
                    } else {
                      add_elems(collection_key, *values + size);
                      SyncClient();
                      check_card(collection_key, size + 1);
                      SyncClient();
                      del_elems(collection_key, *values);
                    }
                    SyncClient();
                    check_card(collection_key, size);
                    ++*values;
                  };
    auto check = [get_check, check_card, collection_key, size, val_size, values, this]
                 (T* curr_vals) {
                   T* it = values;
                   SyncClient();
                   for ( ; it < curr_vals; ++it)
                     get_check(collection_key, it, false);
                   for ( ; it < curr_vals + size; ++it)
                     get_check(collection_key, it, true);
                   for ( ; it < values + val_size; ++it)
                     get_check(collection_key, it, false);
                   SyncClient();
                   check_card(collection_key, size);
                   SyncClient();
                 };
    auto expired = [get_check, collection_key, size, this](T* values) {
                     CheckExpired(collection_key);
                     for (T* it = values; it < values + size; ++it)
                       get_check(collection_key, it, false);
                     SyncClient();
                   };

    // Checking TTL and PERSIST on a persistent collection.
    init(values);
    DoRedisTestInt(__LINE__, {"TTL", key}, -1);
    DoRedisTestInt(__LINE__, {"PTTL", key}, -1);
    DoRedisTestInt(__LINE__, {"PERSIST", key}, 0);
    SyncClient();
    // Checking that modification does not change anything.
    modify(&values);
    DoRedisTestInt(__LINE__, {"TTL", key}, -1);
    DoRedisTestInt(__LINE__, {"PTTL", key}, -1);
    DoRedisTestInt(__LINE__, {"PERSIST", key}, 0);
    check(values);
    SyncClient();
    // Adding TTL and checking that modification does not change anything.
    DoRedisTestInt(__LINE__, {"EXPIRE", key, "7"}, 1);
    modify(&values);
    DoRedisTestInt(__LINE__, {"TTL", key}, 7);
    check(values);
    SyncClient();
    // Checking that everything is still there after some time.
    std::this_thread::sleep_for(3s);
    DoRedisTestInt(__LINE__, {"TTL", key}, 4);
    check(values);
    SyncClient();
    std::this_thread::sleep_for(5s);
    expired(values);
    SyncClient();
    // Checking expiration changes for a later expiration.
    init(values);
    check(values);
    DoRedisTestInt(__LINE__, {"EXPIRE", key, "5"}, 1);
    modify(&values);
    DoRedisTestInt(__LINE__, {"EXPIRE", key, "9"}, 1);
    DoRedisTestInt(__LINE__, {"TTL", key}, 9);
    check(values);
    modify(&values);
    SyncClient();
    std::this_thread::sleep_for(5s);
    DoRedisTestInt(__LINE__, {"TTL", key}, 4);
    check(values);
    modify(&values);
    SyncClient();
    std::this_thread::sleep_for(5s);
    SyncClient();
    expired(values);
    SyncClient();
    // Checking expiration changes for an earlier expiration.
    init(values);
    DoRedisTestInt(__LINE__, {"EXPIRE", key, "5"}, 1);
    modify(&values);
    DoRedisTestInt(__LINE__, {"EXPIRE", key, "3"}, 1);
    DoRedisTestInt(__LINE__, {"TTL", key}, 3);
    check(values);
    modify(&values);
    SyncClient();
    std::this_thread::sleep_for(4s);
    expired(values);
    SyncClient();
    // Checking persistence.
    init(values);
    DoRedisTestInt(__LINE__, {"EXPIRE", key, "6"}, 1);
    SyncClient();
    std::this_thread::sleep_for(3s);
    DoRedisTestInt(__LINE__, {"PERSIST", key}, 1);
    DoRedisTestInt(__LINE__, {"TTL", key}, -1);
    check(values);
    SyncClient();
    std::this_thread::sleep_for(6s);
    check(values);
    SyncClient();
    // Testing zero expiration.
    DoRedisTestInt(__LINE__, {"EXPIRE", key, "0"}, 1);
    expired(values);
    SyncClient();
    // Testing negative expiration.
    init(values);
    DoRedisTestInt(__LINE__, {"EXPIRE", key, "-7"}, 1);
    expired(values);
    SyncClient();
    // Testing SETEX turns the key back into a primitive.
    init(values);
    DoRedisTestOk(__LINE__, {"SETEX", key, "6", "17"});
    SyncClient();
    DoRedisTestBulkString(__LINE__, {"GET", key}, "17");
    SyncClient();
    std::this_thread::sleep_for(7s);
    CheckExpired(&key);
    SyncClient();
    VerifyCallbacks();
  }

  void TestTtlSet(std::string* collection_key, std::string* collection_values, int card) {
    std::function<void(std::string*, std::string*, int)> set_init =
        [this](std::string* key, std::string* values, int size) {
          for (auto it = values; it < values + size; ++it) {
            DoRedisTestInt(__LINE__, {"SADD", *key, *it}, 1);
            SyncClient();
          }
          SyncClient();
        };
    std::function<void(std::string*, std::string*)> set_add =
        [this](std::string* key, std::string* value) {
          DoRedisTestInt(__LINE__, {"SADD", *key, *value}, 1);
        };
    std::function<void(std::string*, std::string*)> set_del =
        [this](std::string* key, std::string* value) {
          DoRedisTestInt(__LINE__, {"SREM", *key, *value}, 1);
        };
    std::function<void(std::string*, std::string*, bool)> set_check =
        [this](std::string* key, std::string* value, bool exists) {
          DoRedisTestInt(__LINE__, {"SISMEMBER", *key, *value}, exists);
        };
    std::function<void(std::string*, int)> set_card =
        [this](std::string* key, int size) {
          DoRedisTestInt(__LINE__, {"SCARD", *key}, size);
        };

    TestTtlCollection(collection_key, collection_values, card,
                      set_init, set_add, set_del, set_check, set_card);
  }

  void TestTtlSortedSet(std::string* collection_key, CollectionEntry* collection_values, int card) {
    std::function<void(std::string*, CollectionEntry*, int)> sorted_set_init =
        [this](std::string* key, CollectionEntry* values, int size) {
          for (auto it = values; it < values + size; ++it) {
            DoRedisTestInt(__LINE__, {"ZADD", *key, std::get<0>(*it), std::get<1>(*it)}, 1);
            SyncClient();
          }
          SyncClient();
        };
    std::function<void(std::string*, CollectionEntry*)> sorted_set_add =
        [this](std::string* key, CollectionEntry* value) {
          DoRedisTestInt(__LINE__, {"ZADD", *key, std::get<0>(*value), std::get<1>(*value)}, 1);
        };
    std::function<void(std::string*, CollectionEntry*)> sorted_set_del =
        [this](std::string* key, CollectionEntry* value) {
          DoRedisTestInt(__LINE__, {"ZREM", *key, std::get<1>(*value)}, 1);
        };
    std::function<void(std::string*, CollectionEntry*, bool)> sorted_set_check =
        [this](std::string* key, CollectionEntry* value, bool exists) {
          if (exists) {
            char buf[20];
            std::snprintf(buf, sizeof(buf), "%.6f", std::stof(std::get<0>(*value)));
            DoRedisTestBulkString(__LINE__, {"ZSCORE", *key, std::get<1>(*value)},
                                  buf);
          } else {
            DoRedisTestNull(__LINE__, {"ZSCORE", *key, std::get<1>(*value)});
          }
        };
    std::function<void(std::string*, int)> sorted_set_card =
        [this](std::string* key, int size) {
          DoRedisTestInt(__LINE__, {"ZCARD", *key}, size);
        };

    TestTtlCollection(collection_key, collection_values, card,
                      sorted_set_init, sorted_set_add, sorted_set_del,
                      sorted_set_check, sorted_set_card);
  }

  void TestTtlHash(std::string* collection_key, CollectionEntry* collection_values, int card) {
    std::function<void(std::string*, CollectionEntry*, int)> hash_init =
        [this](std::string* key, CollectionEntry* values, int size) {
          for (auto it = values; it < values + size; ++it) {
            DoRedisTestInt(__LINE__, {"HSET", *key, std::get<0>(*it), std::get<1>(*it)}, 1);
            SyncClient();
          }
          SyncClient();
        };
    std::function<void(std::string*, CollectionEntry*)> hash_add =
        [this](std::string* key, CollectionEntry* value) {
          DoRedisTestInt(__LINE__, {"HSET", *key, std::get<0>(*value), std::get<1>(*value)}, 1);
        };
    std::function<void(std::string*, CollectionEntry*)> hash_del =
        [this](std::string* key, CollectionEntry* value) {
          DoRedisTestInt(__LINE__, {"HDEL", *key, std::get<0>(*value)}, 1);
        };
    std::function<void(std::string*, CollectionEntry*, bool)> hash_check =
        [this](std::string* key, CollectionEntry* value, bool exists) {
          if (exists)
            DoRedisTestBulkString(__LINE__, {"HGET", *key, std::get<0>(*value)},
                                  std::get<1>(*value));
          else
            DoRedisTestNull(__LINE__, {"HGET", *key, std::get<0>(*value)});
        };
    std::function<void(std::string*, int)> hash_card =
        [this](std::string* key, int size) {
          DoRedisTestInt(__LINE__, {"HLEN", *key}, size);
        };

    TestTtlCollection(collection_key, collection_values, card,
                      hash_init, hash_add, hash_del, hash_check, hash_card);
  }

  void TestAbort(const std::string& command);

 protected:
  std::string int64Max_ = std::to_string(std::numeric_limits<int64_t>::max());
  std::string int64MaxExclusive_ = "(" + int64Max_;
  std::string int64Min_ = std::to_string(std::numeric_limits<int64_t>::min());
  std::string int64MinExclusive_ = "(" + int64Min_;
  uint64_t redis_max_read_buffer_size_ = 512_MB;

 private:
  std::atomic<int> num_callbacks_called_{0};
  int expected_callbacks_called_ = 0;
  Socket client_sock_;
  unique_ptr<RedisServer> server_;
  int redis_server_port_ = 0;
  unique_ptr<FileLock> redis_port_lock_;
  unique_ptr<FileLock> redis_webserver_lock_;
  std::vector<uint8_t> resp_;
  boost::optional<rpc::IoThreadPool> io_thread_pool_;
  std::shared_ptr<RedisClient> test_client_;
};

void TestRedisService::SetUp() {
  FLAGS_redis_service_yb_client_timeout_millis = kDefaultTimeoutMs;
  if (IsTsan()) {
    FLAGS_redis_max_value_size = 1_MB;
    FLAGS_rpc_max_message_size = FLAGS_redis_max_value_size * 4 - 1;
    FLAGS_redis_max_command_size = FLAGS_rpc_max_message_size - 2_KB;
    FLAGS_consensus_max_batch_size_bytes = FLAGS_rpc_max_message_size - 2_KB;
  } else {
#ifndef NDEBUG
    FLAGS_redis_max_value_size = 32_MB;
    FLAGS_rpc_max_message_size = FLAGS_redis_max_value_size * 4 - 1;
    FLAGS_redis_max_command_size = FLAGS_rpc_max_message_size - 2_KB;
    FLAGS_consensus_max_batch_size_bytes = FLAGS_rpc_max_message_size - 2_KB;
    FLAGS_consensus_rpc_timeout_ms = 3000;
#endif
  }
  FLAGS_redis_max_read_buffer_size = redis_max_read_buffer_size_;
  LOG(INFO) << "FLAGS_redis_max_read_buffer_size=" << FLAGS_redis_max_read_buffer_size
            << ", FLAGS_redis_max_queued_bytes=" << FLAGS_redis_max_queued_bytes;

  RedisTableTestBase::SetUp();

  StartServer();
  StartClient();
}

void TestRedisService::StartServer() {
  if (use_external_mini_cluster()) {
    redis_server_port_ = external_mini_cluster()->tablet_server(0)->redis_rpc_port();
    return;
  }

  redis_server_port_ = GetFreePort(&redis_port_lock_);
  RedisServerOptions opts;
  opts.rpc_opts.rpc_bind_addresses = strings::Substitute("0.0.0.0:$0", redis_server_port_);
  // No need to save the webserver port, as we don't plan on using it. Just use a unique free port.
  opts.webserver_opts.port = GetFreePort(&redis_webserver_lock_);
  string fs_root = GetTestPath("RedisServerTest-fsroot");
  opts.fs_opts.wal_paths = {fs_root};
  opts.fs_opts.data_paths = {fs_root};

  auto master_rpc_addrs = master_rpc_addresses_as_strings();
  opts.master_addresses_flag = JoinStrings(master_rpc_addrs, ",");

  server_ = std::make_unique<RedisServer>(opts, mini_cluster()->mini_tablet_server(0)->server());
  LOG(INFO) << "Starting redis server...";
  CHECK_OK(server_->Start());
  LOG(INFO) << "Redis server successfully started.";
}

void TestRedisService::StopServer() {
  if (!server_) {
    return;
  }
  LOG(INFO) << "Shut down redis server...";
  server_->Shutdown();
  server_.reset();
  LOG(INFO) << "Redis server successfully shut down.";
}

void TestRedisService::StartClient() {
  Endpoint remote = RedisProxyEndpoint();
  CHECK_OK(client_sock_.Init(0));
  CHECK_OK(client_sock_.SetNoDelay(false));
  LOG(INFO) << "Connecting to " << remote;
  CHECK_OK(client_sock_.Connect(remote));
  Endpoint local;
  CHECK_OK(client_sock_.GetSocketAddress(&local));
  CHECK_OK(client_sock_.GetPeerAddress(&remote));
  LOG(INFO) << "Connected: " << local << " => " << remote;
}

void TestRedisService::StopClient() { EXPECT_OK(client_sock_.Close()); }

void TestRedisService::RestartClient() {
  StopClient();
  StartClient();
}

void TestRedisService::CloseRedisClient() {
  if (test_client_) {
    test_client_->Disconnect();
    test_client_.reset();
  }
  if (io_thread_pool_) {
    io_thread_pool_->Shutdown();
    io_thread_pool_->Join();
  }
  StopClient();
}

void TestRedisService::TearDown() {
  if (!use_external_mini_cluster()) {
    size_t allocated_sessions = CountSessions(METRIC_redis_allocated_sessions);
    if (!expected_no_sessions_) {
      EXPECT_GT(allocated_sessions, 0); // Check that metric is sane.
    } else {
      EXPECT_EQ(0, allocated_sessions);
    }
    EXPECT_EQ(allocated_sessions, CountSessions(METRIC_redis_available_sessions));
  }

  CloseRedisClient();
  StopServer();
  RedisTableTestBase::TearDown();
}

Status TestRedisService::Send(const std::string& cmd) {
  // Send the command.
  int32_t bytes_written = 0;
  EXPECT_OK(client_sock_.Write(util::to_uchar_ptr(cmd.c_str()), cmd.length(), &bytes_written));

  EXPECT_EQ(cmd.length(), bytes_written);

  return Status::OK();
}

Status TestRedisService::SendCommandAndGetResponse(
    const string& cmd, int expected_resp_length, int timeout_in_millis) {
  RETURN_NOT_OK(Send(cmd));

  // Receive the response.
  MonoTime deadline = MonoTime::Now();
  deadline.AddDelta(MonoDelta::FromMilliseconds(timeout_in_millis));
  size_t bytes_read = 0;
  resp_.resize(expected_resp_length);
  if (expected_resp_length) {
    memset(resp_.data(), 0, expected_resp_length);
  }
  RETURN_NOT_OK(client_sock_.BlockingRecv(
      resp_.data(), expected_resp_length, &bytes_read, deadline));
  resp_.resize(bytes_read);
  if (expected_resp_length != bytes_read) {
    return STATUS(
        IOError, Substitute("Received $1 bytes instead of $2", bytes_read, expected_resp_length));
  }
  return Status::OK();
}

void TestRedisService::SendCommandAndExpectTimeout(const string& cmd) {
  // Don't expect to receive even 1 byte.
  ASSERT_TRUE(SendCommandAndGetResponse(cmd, 1).IsTimedOut());
}

void TestRedisService::SendCommandAndExpectResponse(int line,
    const string& cmd,
    const string& expected,
    bool partial) {
  if (partial) {
    auto seed = GetRandomSeed32();
    std::mt19937_64 rng(seed);
    size_t last = cmd.length() - 2;
    size_t splits = std::uniform_int_distribution<size_t>(1, 10)(rng);
    std::vector<size_t> bounds(splits);
    std::generate(bounds.begin(), bounds.end(), [&rng, last]{
      return std::uniform_int_distribution<size_t>(1, last)(rng);
    });
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());
    size_t p = 0;
    for (auto i : bounds) {
      ASSERT_OK(Send(cmd.substr(p, i - p)));
      p = i;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_OK(SendCommandAndGetResponse(cmd.substr(p), expected.length()));
  } else {
    auto status = SendCommandAndGetResponse(cmd, expected.length());
    if (!status.ok()) {
      LOG(INFO) << "    Sent: " << Slice(cmd).ToDebugString();
      LOG(INFO) << "Received: " << Slice(resp_.data(), resp_.size()).ToDebugString();
      LOG(INFO) << "Expected: " << Slice(expected).ToDebugString();
    }
    ASSERT_OK(status);
  }

  // Verify that the response is as expected.

  std::string response(util::to_char_ptr(resp_.data()), expected.length());
  ASSERT_EQ(expected, response)
                << "Command: " << Slice(cmd).ToDebugString() << std::endl
                << "Originator: " << __FILE__ << ":" << line;
}

template <class Callback>
void TestRedisService::DoRedisTest(int line,
    const std::vector<std::string>& command,
    RedisReplyType reply_type,
    const Callback& callback) {
  expected_callbacks_called_++;
  VLOG(4) << "Testing with line: " << __FILE__ << ":" << line;
  client().Send(command, [this, line, reply_type, callback](const RedisReply& reply) {
    VLOG(4) << "Received response for line: " << __FILE__ << ":" << line << " : "
            << reply.as_string() << ", of type: " << to_underlying(reply.get_type());
    num_callbacks_called_++;
    ASSERT_EQ(reply_type, reply.get_type())
        << "Originator: " << __FILE__ << ":" << line << ", reply: " << reply.ToString();
    callback(reply);
  });
}

void TestRedisService::VerifyCallbacks() {
  ASSERT_EQ(expected_callbacks_called_, num_callbacks_called_.load(std::memory_order_acquire));
}

TEST_F(TestRedisService, SimpleCommandInline) {
  SendCommandAndExpectResponse(__LINE__, "set foo bar\r\n", "+OK\r\n");
}

void TestRedisService::TestAbort(const std::string& command) {
  ASSERT_OK(Send(command));
  std::this_thread::sleep_for(1000ms);
  StopClient();

  // TODO When reactor is shutting down, we cannot notify it that call is responded.
  // It is possible that it could happen not only with debug sleep.
  std::this_thread::sleep_for(2000ms);
}

TEST_F(TestRedisService, AbortDuringProcessing) {
  TestAbort("DEBUGSLEEP 2000\r\n");
}

class TestRedisServiceCleanQueueOnShutdown : public TestRedisService {
 public:
  void SetUp() override {
    saver_.emplace();

    FLAGS_redis_max_concurrent_commands = 1;
    FLAGS_redis_max_batch = 1;
    TestRedisService::SetUp();
  }

  void TearDown() override {
    TestRedisService::TearDown();
    saver_.reset();
  }

 private:
  boost::optional<google::FlagSaver> saver_;
};

TEST_F_EX(TestRedisService, AbortQueueOnShutdown, TestRedisServiceCleanQueueOnShutdown) {
  TestAbort("DEBUGSLEEP 2000\r\nDEBUGSLEEP 999999999\r\n");
}

TEST_F(TestRedisService, AbortBatches) {
  TestAbort("DEBUGSLEEP 2000\r\nSET foo 1\r\nGET foo\r\nDEBUGSLEEP 999999999\r\n");
}

class TestRedisServiceReceiveBufferOverflow : public TestRedisService {
 public:
  void SetUp() override {
    saver_.emplace();

    FLAGS_redis_max_concurrent_commands = 1;
    FLAGS_redis_max_batch = 1;
    // TODO FLAGS_rpc_initial_buffer_size = 128;
    redis_max_read_buffer_size_changer_.Init(128, &redis_max_read_buffer_size_);
    FLAGS_redis_max_queued_bytes = 0;
    TestRedisService::SetUp();
  }

  void TearDown() override {
    TestRedisService::TearDown();
    redis_max_read_buffer_size_changer_.Reset();
    saver_.reset();
  }

 private:
  boost::optional<google::FlagSaver> saver_;
  ValueChanger<uint64_t> redis_max_read_buffer_size_changer_;
};

TEST_F_EX(TestRedisService, ReceiveBufferOverflow, TestRedisServiceReceiveBufferOverflow) {
  auto key = std::string(FLAGS_redis_max_read_buffer_size - 12, 'X');
  SendCommandAndExpectResponse(
      __LINE__, Format("DEBUGSLEEP 2000\r\nSET key $0\r\n", key), "+OK\r\n+OK\r\n");

  SendCommandAndExpectResponse(
      __LINE__,
      Format("DEBUGSLEEP 2000\r\nSET key1 $0\r\nSET key2 $0\r\n", key, key),
      "+OK\r\n+OK\r\n+OK\r\n");
}

class TestTooBigCommand : public TestRedisService {
  void SetUp() override {
    FLAGS_redis_rpc_block_size = 32;
    redis_max_read_buffer_size_changer_.Init(1024, &redis_max_read_buffer_size_);
    TestRedisService::SetUp();
  }

  void TearDown() override {
    TestRedisService::TearDown();
    redis_max_read_buffer_size_changer_.Reset();
  }

 private:
  ValueChanger<uint64_t> redis_max_read_buffer_size_changer_;
};

TEST_F_EX(TestRedisService, TooBigCommand, TestTooBigCommand) {
  std::string small_key(FLAGS_redis_max_read_buffer_size / 2, 'X');
  SendCommandAndExpectResponse(
      __LINE__, Format("SET key1 $0\r\nSET key2 $0\r\n", small_key), "+OK\r\n+OK\r\n");
  std::string big_key(FLAGS_redis_max_read_buffer_size, 'X');
  std::string key_suffix(FLAGS_redis_rpc_block_size, 'Y');
  auto status = SendCommandAndGetResponse(Format("SET key$0 $1\r\n", key_suffix, big_key), 1);
  ASSERT_TRUE(status.IsNetworkError()) << "Status: " << status;
}

TEST_F(TestRedisService, HugeCommandInline) {
  // Set a larger timeout for the yql layer : 1 min vs 10 min for tsan/asan.
  FLAGS_redis_service_yb_client_timeout_millis = 6 * kDefaultTimeoutMs;

  LOG(INFO) << "Creating a value of size " << FLAGS_redis_max_value_size;
  std::string value(FLAGS_redis_max_value_size, 'T');
  DoRedisTestOk(__LINE__, {"SET", "foo", value});
  SyncClient();
  DoRedisTestBulkString(__LINE__, {"GET", "foo"}, value);
  SyncClient();
  DoRedisTestOk(__LINE__, {"SET", "foo", "Test"});
  DoRedisTestBulkString(__LINE__, {"GET", "foo"}, "Test");
  SyncClient();
  DoRedisTestExpectError(__LINE__, {"SET", "foo", "Too much" + value});
  SyncClient();
  DoRedisTestInt(__LINE__, {"HSET", "map_key", "subkey1", value}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"HSET", "map_key", "subkey2", value}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"HSET", "map_key", "subkey3", value}, 1);
  SyncClient();
  DoRedisTestArray(__LINE__, {"HGETALL", "map_key"}, {"subkey1", value, "subkey2", value,
      "subkey3", value});
  SyncClient();
  DoRedisTestInt(__LINE__, {"HSET", "map_key", "subkey4", value}, 1);
  DoRedisTestExpectError(__LINE__, {"HGETALL", "map_key"});
  SyncClient();
  DoRedisTestInt(__LINE__, {"DEL", "map_key"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"DEL", "foo"}, 1);
  SyncClient();
  value[0]  = 'A';
  DoRedisTestOk(
      __LINE__, {"HMSET", "map_key1", "subkey1", value, "subkey2", value, "subkey3", value});
  SyncClient();
  DoRedisTestArray(
      __LINE__, {"HGETALL", "map_key1"}, {"subkey1", value, "subkey2", value, "subkey3", value});
  SyncClient();
  value[0] = 'B';
  DoRedisTestExpectError(
      __LINE__,
      {"HMSET", "map_key1", "subkey1", value, "subkey2", value, "subkey3", value,
          "subkey4", value});
  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, SimpleCommandMulti) {
  SendCommandAndExpectResponse(
      __LINE__, "*3\r\n$3\r\nset\r\n$3\r\nfoo\r\n$4\r\nTEST\r\n", "+OK\r\n");
}

TEST_F(TestRedisService, BatchedCommandsInline) {
  SendCommandAndExpectResponse(
      __LINE__,
      "set a 5\r\nset foo bar\r\nget foo\r\nget a\r\n",
      "+OK\r\n+OK\r\n$3\r\nbar\r\n$1\r\n5\r\n");
}

TEST_F(TestRedisService, TestTimedoutInQueue) {
  FLAGS_redis_max_batch = 1;
  SetAtomicFlag(true, &FLAGS_TEST_enable_backpressure_mode_for_testing);

  DoRedisTestOk(__LINE__, {"SET", "foo", "value"});
  DoRedisTestBulkString(__LINE__, {"GET", "foo"}, "value");
  DoRedisTestOk(__LINE__, {"SET", "foo", "Test"});

  // All calls past this call should fail.
  DoRedisTestOk(__LINE__, {"DEBUGSLEEP", yb::ToString(FLAGS_max_time_in_queue_ms)});

  const string expected_message =
      "The server is overloaded. Call waited in the queue past max_time_in_queue.";
  DoRedisTestExpectError(__LINE__, {"SET", "foo", "Test"}, expected_message);
  DoRedisTestExpectError(__LINE__, {"GET", "foo"}, expected_message);
  DoRedisTestExpectError(__LINE__, {"DEBUGSLEEP", "2000"}, expected_message);

  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, BatchedCommandsInlinePartial) {
  for (int i = 0; i != 1000; ++i) {
    ASSERT_NO_FATAL_FAILURE(
        SendCommandAndExpectResponse(
            __LINE__,
            "set a 5\r\nset foo bar\r\nget foo\r\nget a\r\n",
            "+OK\r\n+OK\r\n$3\r\nbar\r\n$1\r\n5\r\n",
            /* partial */ true)
    );
  }
}

namespace {

class TestRedisServicePipelined : public TestRedisService {
 public:
  void SetUp() override {
    FLAGS_redis_safe_batch = false;
    FLAGS_redis_max_concurrent_commands = FLAGS_test_redis_max_concurrent_commands;
    FLAGS_redis_max_batch = FLAGS_test_redis_max_batch;
    TestRedisService::SetUp();
  }
};

#ifndef THREAD_SANITIZER
const size_t kPipelineKeys = 1000;
#else
const size_t kPipelineKeys = 100;
#endif

size_t ValueForKey(size_t key) {
  return key * 2;
}

std::string PipelineSetCommand() {
  std::string command;
  for (size_t i = 0; i != kPipelineKeys; ++i) {
    command += yb::Format("set $0 $1\r\n", i, ValueForKey(i));
  }
  return command;
}

std::string PipelineSetResponse() {
  std::string response;
  for (size_t i = 0; i != kPipelineKeys; ++i) {
    response += "+OK\r\n";
  }
  return response;
}

std::string PipelineGetCommand() {
  std::string command;
  for (size_t i = 0; i != kPipelineKeys; ++i) {
    command += yb::Format("get $0\r\n", i);
  }
  return command;
}

std::string PipelineGetResponse() {
  std::string response;
  for (size_t i = 0; i != kPipelineKeys; ++i) {
    std::string value = std::to_string(ValueForKey(i));
    response += yb::Format("$$$0\r\n$1\r\n", value.length(), value);
  }
  return response;
}

} // namespace

TEST_F_EX(TestRedisService, Pipeline, TestRedisServicePipelined) {
  auto start = std::chrono::steady_clock::now();
  SendCommandAndExpectResponse(__LINE__, PipelineSetCommand(), PipelineSetResponse());
  auto mid = std::chrono::steady_clock::now();
  SendCommandAndExpectResponse(__LINE__, PipelineGetCommand(), PipelineGetResponse());
  auto end = std::chrono::steady_clock::now();
  auto set_time = std::chrono::duration_cast<std::chrono::milliseconds>(mid - start);
  auto get_time = std::chrono::duration_cast<std::chrono::milliseconds>(end - mid);
  LOG(INFO) << yb::Format("Unsafe set: $0ms, get: $1ms", set_time.count(), get_time.count());
}

TEST_F_EX(TestRedisService, PipelinePartial, TestRedisServicePipelined) {
  SendCommandAndExpectResponse(__LINE__,
      PipelineSetCommand(),
      PipelineSetResponse(),
      true /* partial */);
  SendCommandAndExpectResponse(__LINE__,
      PipelineGetCommand(),
      PipelineGetResponse(),
      true /* partial */);
}

namespace {

class BatchGenerator {
 public:
  explicit BatchGenerator(bool collisions) : collisions_(collisions), random_(293462970) {}

  std::pair<std::string, std::string> Generate() {
    new_values_.clear();
    requested_keys_.clear();
    std::string command, response;
    size_t size = size_distribution_(random_);
    for (size_t j = 0; j != size; ++j) {
      bool get = !keys_.empty() && (bool_distribution_(random_) != 0);
      if (get) {
        int key = keys_[std::uniform_int_distribution<size_t>(0, keys_.size() - 1)(random_)];
        if (!collisions_ && new_values_.count(key)) {
          continue;
        }
        command += yb::Format("get $0\r\n", key);
        auto value = std::to_string(values_[key]);
        response += yb::Format("$$$0\r\n$1\r\n", value.length(), value);
        requested_keys_.insert(key);
      } else {
        int value = value_distribution_(random_);
        for (;;) {
          int key = key_distribution_(random_);
          if (collisions_) {
            StoreValue(key, value);
          } else if(requested_keys_.count(key) || !new_values_.emplace(key, value).second) {
            continue;
          }
          command += yb::Format("set $0 $1\r\n", key, value);
          response += "+OK\r\n";
          break;
        }
      }
    }

    for (const auto& p : new_values_) {
      StoreValue(p.first, p.second);
    }
    return std::make_pair(std::move(command), std::move(response));
  }
 private:
  void StoreValue(int key, int value) {
    auto it = values_.find(key);
    if (it == values_.end()) {
      values_.emplace(key, value);
      keys_.push_back(key);
    } else {
      it->second = value;
    }
  }

  static constexpr size_t kMinSize = 500;
  static constexpr size_t kMaxSize = kMinSize + 511;
  static constexpr int kMinKey = 0;
  static constexpr int kMaxKey = 1023;
  static constexpr int kMinValue = 0;
  static constexpr int kMaxValue = 1023;

  const bool collisions_;
  std::mt19937_64 random_;
  std::uniform_int_distribution<int> bool_distribution_{0, 1};
  std::uniform_int_distribution<size_t> size_distribution_{kMinSize, kMaxSize};
  std::uniform_int_distribution<int> key_distribution_{kMinKey, kMaxKey};
  std::uniform_int_distribution<int> value_distribution_{kMinValue, kMaxValue};

  std::unordered_map<int, int> values_;
  std::unordered_map<int, int> new_values_;
  std::unordered_set<int> requested_keys_;
  std::vector<int> keys_;
};

} // namespace

TEST_F_EX(TestRedisService, MixedBatch, TestRedisServicePipelined) {
  constexpr size_t kBatches = 50;
  BatchGenerator generator(false);
  for (size_t i = 0; i != kBatches; ++i) {
    auto batch = generator.Generate();
    SendCommandAndExpectResponse(__LINE__, batch.first, batch.second);
  }
}

class TestRedisServiceSafeBatch : public TestRedisService {
 public:
  void SetUp() override {
    FLAGS_redis_max_concurrent_commands = 1;
    FLAGS_redis_max_batch = FLAGS_test_redis_max_batch;
    FLAGS_redis_safe_batch = true;
    TestRedisService::SetUp();
  }
};

TEST_F_EX(TestRedisService, SafeMixedBatch, TestRedisServiceSafeBatch) {
  constexpr size_t kBatches = 50;
  BatchGenerator generator(true);
  std::vector<decltype(generator.Generate())> batches;
  for (size_t i = 0; i != kBatches; ++i) {
    batches.push_back(generator.Generate());
  }
  auto start = std::chrono::steady_clock::now();
  for (const auto& batch : batches) {
    SendCommandAndExpectResponse(__LINE__, batch.first, batch.second);
  }
  auto total = std::chrono::steady_clock::now() - start;
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(total).count();
  LOG(INFO) << Format("Total: $0ms, average: $1ms", ms, ms / kBatches);
}

TEST_F_EX(TestRedisService, SafeBatchPipeline, TestRedisServiceSafeBatch) {
  auto start = std::chrono::steady_clock::now();
  SendCommandAndExpectResponse(__LINE__, PipelineSetCommand(), PipelineSetResponse());
  auto mid = std::chrono::steady_clock::now();
  SendCommandAndExpectResponse(__LINE__, PipelineGetCommand(), PipelineGetResponse());
  auto end = std::chrono::steady_clock::now();
  auto set_time = std::chrono::duration_cast<std::chrono::milliseconds>(mid - start);
  auto get_time = std::chrono::duration_cast<std::chrono::milliseconds>(end - mid);
  LOG(INFO) << yb::Format("Safe set: $0ms, get: $1ms", set_time.count(), get_time.count());
}

TEST_F(TestRedisService, BatchedCommandMulti) {
  SendCommandAndExpectResponse(
      __LINE__,
      "*3\r\n$3\r\nset\r\n$3\r\nfoo\r\n$4\r\nTEST\r\n"
          "*3\r\n$3\r\nset\r\n$3\r\nfoo\r\n$4\r\nTEST\r\n"
          "*3\r\n$3\r\nset\r\n$3\r\nfoo\r\n$4\r\nTEST\r\n",
      "+OK\r\n+OK\r\n+OK\r\n");
}

TEST_F(TestRedisService, BatchedCommandMultiPartial) {
  for (int i = 0; i != 1000; ++i) {
    ASSERT_NO_FATAL_FAILURE(
        SendCommandAndExpectResponse(
            __LINE__,
            "*3\r\n$3\r\nset\r\n$3\r\nfoo\r\n$5\r\nTEST1\r\n"
                "*3\r\n$3\r\nset\r\n$3\r\nfoo\r\n$5\r\nTEST2\r\n"
                "*3\r\n$3\r\nset\r\n$3\r\nfoo\r\n$5\r\nTEST3\r\n"
                "*2\r\n$3\r\nget\r\n$3\r\nfoo\r\n",
            "+OK\r\n+OK\r\n+OK\r\n$5\r\nTEST3\r\n",
            /* partial */ true)
    );
  }
}

TEST_F(TestRedisService, IncompleteCommandInline) {
  expected_no_sessions_ = true;
  SendCommandAndExpectTimeout("TEST");
}

TEST_F(TestRedisService, MalformedCommandsFollowedByAGoodOne) {
  expected_no_sessions_ = true;
  ASSERT_NOK(SendCommandAndGetResponse("*3\r\n.1\r\n", 1));
  RestartClient();
  ASSERT_NOK(SendCommandAndGetResponse("*0\r\n.2\r\n", 1));
  RestartClient();
  ASSERT_NOK(SendCommandAndGetResponse("*-4\r\n.3\r\n", 1));
  RestartClient();
  SendCommandAndExpectResponse(__LINE__, "*2\r\n$4\r\necho\r\n$3\r\nfoo\r\n", "$3\r\nfoo\r\n");
}

namespace {

void TestBadCommand(std::string command, TestRedisService* test) {
  ASSERT_NOK(test->SendCommandAndGetResponse(command, 1)) << "Command: " << command;
  test->RestartClient();

  command.erase(std::remove(command.begin(), command.end(), '\n'), command.end());

  if (!command.empty()) {
    ASSERT_NOK(test->SendCommandAndGetResponse(command, 1)) << "Command: " << command;
    test->RestartClient();
  }
}

} // namespace

TEST_F(TestRedisService, BadCommand) {
  expected_no_sessions_ = true;

  TestBadCommand("\n", this);
  TestBadCommand(" \r\n", this);
  TestBadCommand("*\r\n9\r\n", this);
  TestBadCommand("1\r\n\r\n", this);
  TestBadCommand("1\r\n \r\n", this);
  TestBadCommand("1\r\n*0\r\n", this);
}

TEST_F(TestRedisService, BadRandom) {
  expected_no_sessions_ = true;
  const std::string allowed = " -$*\r\n0123456789";
  std::string command;
  constexpr size_t kTotalProbes = 100;
  constexpr size_t kMinCommandLength = 1;
  constexpr size_t kMaxCommandLength = 100;
  constexpr int kTimeoutInMillis = 250;
  for (int i = 0; i != kTotalProbes; ++i) {
    size_t len = RandomUniformInt(kMinCommandLength, kMaxCommandLength);
    command.clear();
    for (size_t idx = 0; idx != len; ++idx) {
      command += RandomElement(allowed);
      if (command[command.length() - 1] == '\r') {
        command += '\n';
      }
    }

    LOG(INFO) << "Command: " << command;
    auto status = SendCommandAndGetResponse(command, 1, kTimeoutInMillis);
    // We don't care about status here, because even usually it fails,
    // sometimes it has non empty response.
    // Our main goal is to test that server does not crash.
    LOG(INFO) << "Status: " << status;

    RestartClient();
  }
}

TEST_F(TestRedisService, IncompleteCommandMulti) {
  expected_no_sessions_ = true;
  SendCommandAndExpectTimeout("*3\r\n$3\r\nset\r\n$3\r\nfoo\r\n$4\r\nTE");
}

TEST_F(TestRedisService, Echo) {
  expected_no_sessions_ = true;
  SendCommandAndExpectResponse(__LINE__, "*2\r\n$4\r\necho\r\n$3\r\nfoo\r\n", "$3\r\nfoo\r\n");
  SendCommandAndExpectResponse(
      __LINE__, "*2\r\n$4\r\necho\r\n$8\r\nfoo bar \r\n", "$8\r\nfoo bar \r\n");
  SendCommandAndExpectResponse(
      __LINE__,
      EncodeAsArray({  // The request is sent as a multi bulk array.
          "echo"s,
          "foo bar"s
      }),
      EncodeAsBulkString("foo bar")  // The response is in the bulk string format.
  );
}

TEST_F(TestRedisService, TestSetOnly) {
  SendCommandAndExpectResponse(
      __LINE__, "*3\r\n$3\r\nset\r\n$3\r\nfoo\r\n$4\r\nTEST\r\n", "+OK\r\n");
  SendCommandAndExpectResponse(
      __LINE__, "*3\r\n$3\r\nset\r\n$4\r\nfool\r\n$4\r\nBEST\r\n", "+OK\r\n");
}

TEST_F(TestRedisService, TestCaseInsensitiveness) {
  SendCommandAndExpectResponse(
      __LINE__, "*3\r\n$3\r\nset\r\n$3\r\nfoo\r\n$4\r\nTEST\r\n", "+OK\r\n");
  SendCommandAndExpectResponse(
      __LINE__, "*3\r\n$3\r\nSet\r\n$3\r\nfoo\r\n$4\r\nTEST\r\n", "+OK\r\n");
  SendCommandAndExpectResponse(
      __LINE__, "*3\r\n$3\r\nsEt\r\n$3\r\nfoo\r\n$4\r\nTEST\r\n", "+OK\r\n");
  SendCommandAndExpectResponse(
      __LINE__, "*3\r\n$3\r\nseT\r\n$3\r\nfoo\r\n$4\r\nTEST\r\n", "+OK\r\n");
  SendCommandAndExpectResponse(
      __LINE__, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$4\r\nTEST\r\n", "+OK\r\n");
}

TEST_F(TestRedisService, TestSetThenGet) {
  SendCommandAndExpectResponse(__LINE__,
      "*3\r\n$3\r\nset\r\n$3\r\nfoo\r\n$4\r\nTEST\r\n", "+OK\r\n");
  SendCommandAndExpectResponse(__LINE__, "*2\r\n$3\r\nget\r\n$3\r\nfoo\r\n", "$4\r\nTEST\r\n");
  SendCommandAndExpectResponse(
      __LINE__,
      EncodeAsArray({  // The request is sent as a multi bulk array.
          "set"s,
          "name"s,
          "yugabyte"s
      }),
      EncodeAsSimpleString("OK")  // The response is in the simple string format.
  );
  SendCommandAndExpectResponse(
      __LINE__,
      EncodeAsArray({  // The request is sent as a multi bulk array.
          "get"s,
          "name"s
      }),
      EncodeAsBulkString("yugabyte")  // The response is in the bulk string format.
  );
}

TEST_F(TestRedisService, TestUsingOpenSourceClient) {
  DoRedisTestOk(__LINE__, {"SET", "hello", "42"});

  DoRedisTest(__LINE__, {"DECRBY", "hello", "12"},
      RedisReplyType::kError, // TODO: fix error handling
      [](const RedisReply &reply) {
        // TBD: ASSERT_EQ(30, reply.as_integer());
      });

  DoRedisTestBulkString(__LINE__, {"GET", "hello"}, "42");
  DoRedisTestOk(__LINE__, {"SET", "world", "72"});

  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestBinaryUsingOpenSourceClient) {
  const std::string kFooValue = "\001\002\r\n\003\004"s;
  const std::string kBarValue = "\013\010\000"s;

  DoRedisTestOk(__LINE__, {"SET", "foo", kFooValue});
  DoRedisTestBulkString(__LINE__, {"GET", "foo"}, kFooValue);
  DoRedisTestOk(__LINE__, {"SET", "bar", kBarValue});
  DoRedisTestBulkString(__LINE__, {"GET", "bar"}, kBarValue);

  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestSingleCommand) {
  DoRedisTestOk(__LINE__, {"SET", "k1", ""});
  DoRedisTestInt(__LINE__, {"HSET", "k2", "s1", ""}, 1);

  SyncClient();

  DoRedisTestBulkString(__LINE__, {"GET", "k1"}, "");
  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestEmptyValue) {
  DoRedisTestOk(__LINE__, {"SET", "k1", ""});
  DoRedisTestInt(__LINE__, {"HSET", "k2", "s1", ""}, 1);

  SyncClient();

  DoRedisTestBulkString(__LINE__, {"GET", "k1"}, "");
  DoRedisTestBulkString(__LINE__, {"HGET", "k2", "s1"}, "");

  SyncClient();
  VerifyCallbacks();
}

void ConnectWithPassword(
    TestRedisService* test, const char* password, bool auth_should_succeed,
    bool get_should_succeed) {
  auto rc1 = std::make_shared<RedisClient>("127.0.0.1", test->server_port());
  test->UseClient(rc1);

  if (auth_should_succeed) {
    if (password != nullptr) test->DoRedisTestOk(__LINE__, {"AUTH", password});
  } else {
    if (password != nullptr) test->DoRedisTestExpectError(__LINE__, {"AUTH", password});
  }

  if (get_should_succeed) {
    test->DoRedisTestOk(__LINE__, {"SET", "k1", "5"});
    test->DoRedisTestBulkString(__LINE__, {"GET", "k1"}, "5");
  } else {
    test->DoRedisTestExpectError(__LINE__, {"SET", "k1", "5"});
    test->DoRedisTestExpectError(__LINE__, {"GET", "k1"});
  }

  test->SyncClient();
  test->UseClient(nullptr);
}

TEST_F(TestRedisService, TestSelect) {
  auto rc1 = std::make_shared<RedisClient>("127.0.0.1", server_port());
  auto rc2 = std::make_shared<RedisClient>("127.0.0.1", server_port());
  auto rc3 = std::make_shared<RedisClient>("127.0.0.1", server_port());

  const string default_db("0");
  const string second_db("2");

  UseClient(rc1);
  DoRedisTestOk(__LINE__, {"SET", "key", "v1"});
  SyncClient();

  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "v1");
  SyncClient();

  // Select without creating a db should fail.
  DoRedisTestExpectError(__LINE__, {"SELECT", second_db.c_str()});
  SyncClient();

  // The connection would be closed upon a bad Select.
  DoRedisTestExpectError(__LINE__, {"PING"});
  SyncClient();

  // Use a different client.
  UseClient(rc2);
  // Get the value from the default_db.
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "v1");
  SyncClient();

  // Create DB.
  DoRedisTestOk(__LINE__, {"CREATEDB", second_db.c_str()});
  SyncClient();

  // Select should now go through.
  DoRedisTestOk(__LINE__, {"SELECT", second_db.c_str()});
  SyncClient();

  // Get should be empty.
  DoRedisTestNull(__LINE__, {"GET", "key"});
  SyncClient();
  // Set a diffferent value
  DoRedisTestOk(__LINE__, {"SET", "key", "v2"});
  SyncClient();
  // Get that value
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "v2");
  SyncClient();
  // Select the original db and get the value.
  DoRedisTestOk(__LINE__, {"SELECT", default_db.c_str()});
  SyncClient();
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "v1");
  SyncClient();

  UseClient(rc3);
  // By default we should get the value from db-0
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "v1");
  // Select second db.
  DoRedisTestOk(__LINE__, {"SELECT", second_db.c_str()});
  // Get that value
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "v2");
  SyncClient();

  // List DB.
  DoRedisTestArray(__LINE__, {"LISTDB"}, {default_db, second_db});
  SyncClient();

  // Delete DB.
  DoRedisTestOk(__LINE__, {"DeleteDB", second_db.c_str()});
  SyncClient();
  // Expect to not be able to read the value.
  DoRedisTestExpectError(__LINE__, {"GET", "key"});
  SyncClient();
  // Expect to not be able to read the value.
  DoRedisTestExpectError(__LINE__, {"SET", "key", "v2"});
  SyncClient();

  // List DB.
  DoRedisTestArray(__LINE__, {"LISTDB"}, {default_db});
  SyncClient();

  rc1->Disconnect();
  rc2->Disconnect();
  rc3->Disconnect();

  UseClient(nullptr);
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestTruncate) {
  const string default_db("0");
  const string second_db("2");

  DoRedisTestOk(__LINE__, {"SET", "key", "v1"});
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "v1");
  SyncClient();

  // Create DB.
  DoRedisTestOk(__LINE__, {"CREATEDB", second_db.c_str()});
  // Select should now go through.
  DoRedisTestOk(__LINE__, {"SELECT", second_db.c_str()});
  SyncClient();

  // Set a diffferent value
  DoRedisTestOk(__LINE__, {"SET", "key", "v2"});
  // Get that value
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "v2");
  SyncClient();

  // Select the original db and get the value.
  DoRedisTestOk(__LINE__, {"SELECT", default_db.c_str()});
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "v1");
  SyncClient();

  // Flush the default_db
  DoRedisTestOk(__LINE__, {"FLUSHDB"});

  // Get should be empty.
  DoRedisTestOk(__LINE__, {"SELECT", default_db.c_str()});
  DoRedisTestNull(__LINE__, {"GET", "key"});
  SyncClient();
  DoRedisTestOk(__LINE__, {"SELECT", second_db.c_str()});
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "v2");
  SyncClient();

  DoRedisTestOk(__LINE__, {"SELECT", default_db.c_str()});
  DoRedisTestOk(__LINE__, {"SET", "key", "v1"});
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "v1");
  SyncClient();

  // Flush the default_db
  DoRedisTestOk(__LINE__, {"FLUSHALL"});

  DoRedisTestNull(__LINE__, {"GET", "key"});
  SyncClient();

  DoRedisTestOk(__LINE__, {"SELECT", default_db.c_str()});
  DoRedisTestNull(__LINE__, {"GET", "key"});
  SyncClient();
  DoRedisTestOk(__LINE__, {"SELECT", second_db.c_str()});
  DoRedisTestNull(__LINE__, {"GET", "key"});
  SyncClient();

  // List DB.
  DoRedisTestArray(__LINE__, {"LISTDB"}, {default_db, second_db});
  SyncClient();

  VerifyCallbacks();
}

TEST_F(TestRedisService, TestDeleteDB) {
  const string default_db("0");
  const string second_db("2");

  DoRedisTestOk(__LINE__, {"SET", "key", "v1"});
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "v1");
  SyncClient();

  // Create DB.
  DoRedisTestOk(__LINE__, {"CREATEDB", second_db.c_str()});
  // Select should now go through.
  DoRedisTestOk(__LINE__, {"SELECT", second_db.c_str()});
  SyncClient();

  // Set a diffferent value
  DoRedisTestOk(__LINE__, {"SET", "key", "v2"});
  // Get that value
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "v2");
  SyncClient();

  // Delete and recreate the DB.
  // List DB.
  DoRedisTestArray(__LINE__, {"LISTDB"}, {default_db, second_db});
  SyncClient();
  DoRedisTestOk(__LINE__, {"DELETEDB", second_db.c_str()});
  SyncClient();
  DoRedisTestArray(__LINE__, {"LISTDB"}, {default_db});
  SyncClient();
  DoRedisTestOk(__LINE__, {"CREATEDB", second_db.c_str()});
  SyncClient();
  DoRedisTestArray(__LINE__, {"LISTDB"}, {default_db, second_db});
  SyncClient();
  // With retries we should succeed immediately.
  DoRedisTestNull(__LINE__, {"GET", "key"});
  SyncClient();
  // Set a diffferent value
  DoRedisTestOk(__LINE__, {"SET", "key", "v2"});
  SyncClient();
  // Get that value
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "v2");
  SyncClient();

  // Delete and recreate the DB. Followed by a write.
  DoRedisTestArray(__LINE__, {"LISTDB"}, {default_db, second_db});
  SyncClient();
  DoRedisTestOk(__LINE__, {"DELETEDB", second_db.c_str()});
  SyncClient();
  DoRedisTestArray(__LINE__, {"LISTDB"}, {default_db});
  SyncClient();
  DoRedisTestOk(__LINE__, {"CREATEDB", second_db.c_str()});
  SyncClient();
  // Set a value
  DoRedisTestOk(__LINE__, {"SET", "key", "v3"});
  SyncClient();
  // Get that value
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "v3");
  SyncClient();

  // Delete and recreate the DB. Followed by a local op.
  DoRedisTestArray(__LINE__, {"LISTDB"}, {default_db, second_db});
  SyncClient();
  DoRedisTestOk(__LINE__, {"DELETEDB", second_db.c_str()});
  SyncClient();
  DoRedisTestArray(__LINE__, {"LISTDB"}, {default_db});
  SyncClient();
  DoRedisTestOk(__LINE__, {"CREATEDB", second_db.c_str()});
  SyncClient();
  DoRedisTestBulkString(__LINE__, {"PING", "cmd2"}, "cmd2");
  SyncClient();
  DoRedisTestBulkString(__LINE__, {"PING", "cmd2"}, "cmd2");
  SyncClient();

  VerifyCallbacks();
}

TEST_F(TestRedisService, TestMonitor) {
  constexpr uint32 kDelayMs = NonTsanVsTsan(100, 1000);
  expected_no_sessions_ = true;
  auto rc1 = std::make_shared<RedisClient>("127.0.0.1", server_port());
  auto rc2 = std::make_shared<RedisClient>("127.0.0.1", server_port());
  auto mc1 = std::make_shared<RedisClient>("127.0.0.1", server_port());
  auto mc2 = std::make_shared<RedisClient>("127.0.0.1", server_port());

  UseClient(rc1);
  DoRedisTestBulkString(__LINE__, {"PING", "cmd1"}, "cmd1");  // Excluded from both mc1 and mc2.
  SyncClient();

  // Check number of monitoring clients.
  ASSERT_EQ(0, CountSessions(METRIC_redis_monitoring_clients));

  UseClient(mc1);
  DoRedisTestOk(__LINE__, {"MONITOR"});
  SyncClient();

  // Wait for the server to realize that the connection is closed.
  std::this_thread::sleep_for(std::chrono::milliseconds(kDelayMs));
  // Check number of monitoring clients.
  ASSERT_EQ(1, CountSessions(METRIC_redis_monitoring_clients));

  UseClient(rc2);
  DoRedisTestBulkString(__LINE__, {"PING", "cmd2"}, "cmd2");  // Included in mc1.
  SyncClient();

  UseClient(mc2);
  DoRedisTestOk(__LINE__, {"MONITOR"});
  SyncClient();

  // Wait for the server to realize that the connection is closed.
  std::this_thread::sleep_for(std::chrono::milliseconds(kDelayMs));
  // Check number of monitoring clients.
  ASSERT_EQ(2, CountSessions(METRIC_redis_monitoring_clients));

  UseClient(rc1);
  const string really_long(100, 'x');
  const string response_ending = string("\"PING\" \"").append(really_long).append("\"");
  DoRedisTestBulkString(__LINE__, {"PING", really_long}, really_long); // Included in mc1 and mc2.
  SyncClient();

  UseClient(mc1);
  // Check the responses for monitor on mc1.
  // Responses are of the format
  // <TS> {<db-id> <client-ip>:<port>} "CMD" "ARG1" ....
  // We will check for the responses to end in "CMD" "ARG1"
  DoRedisTestExpectSimpleStringEndingWith(__LINE__, {}, "\"PING\" \"cmd2\"");
  DoRedisTestExpectSimpleStringEndingWith(__LINE__, {}, response_ending);
  SyncClient();

  UseClient(mc2);
  // Check the responses for monitor on mc2.
  DoRedisTestExpectSimpleStringEndingWith(__LINE__, {}, response_ending);
  SyncClient();

  // Check number of monitoring clients.
  ASSERT_EQ(2, CountSessions(METRIC_redis_monitoring_clients));

  // Close one monitoring clients.
  mc1->Disconnect();
  // Wait for the server to realize that the connection is closed.
  std::this_thread::sleep_for(std::chrono::milliseconds(kDelayMs));

  // Check number of monitoring clients.
  UseClient(rc1);
  DoRedisTestBulkString(__LINE__, {"PING", "test"}, "test");
  SyncClient();

  // Wait for the server to realize that the connection is closed.
  std::this_thread::sleep_for(std::chrono::milliseconds(kDelayMs));
  // Check number of monitoring clients.
  ASSERT_EQ(1, CountSessions(METRIC_redis_monitoring_clients));

  mc2->Disconnect();
  // Wait for the server to realize that the connection is closed.
  std::this_thread::sleep_for(std::chrono::milliseconds(kDelayMs));

  // Check number of monitoring clients.
  UseClient(rc1);
  DoRedisTestBulkString(__LINE__, {"PING", "test"}, "test");
  SyncClient();

  // Wait for the server to realize that the connection is closed.
  std::this_thread::sleep_for(std::chrono::milliseconds(kDelayMs));

  ASSERT_EQ(0, CountSessions(METRIC_redis_monitoring_clients));

  UseClient(nullptr);
  VerifyCallbacks();
}

void TestSubscribe(
    TestRedisService* tester,
    std::shared_ptr<RedisClient> ps0,  // Used for PubSub command
    std::shared_ptr<RedisClient> sc1,  // Used for Subscribe
    std::shared_ptr<RedisClient> sc2,
    std::shared_ptr<RedisClient> sc3,
    std::shared_ptr<RedisClient> pc1,  // Used for Publish
    std::shared_ptr<RedisClient> pc2) {
  const string topic1 = "topic1", topic2 = "topic2";
  const string msg1 = "msg1", msg2 = "msg2";

  if (ps0) {
    tester->UseClient(ps0);
    tester->DoRedisTestArray(__LINE__, {"pubsub", "channels"}, {});
    tester->DoRedisTestResultsArray(
        __LINE__, {"pubsub", "numsub", topic1, topic2},
        {RedisReply(RedisReplyType::kString, topic1), RedisReply(0),
         RedisReply(RedisReplyType::kString, topic2), RedisReply(0)});
    tester->DoRedisTestInt(__LINE__, {"pubsub", "numpat"}, 0);
    tester->SyncClient();
  }

  tester->UseClient(sc1);
  tester->DoRedisTestResultsArray(
      __LINE__, {"SUBSCRIBE", topic1},
      {RedisReply(RedisReplyType::kString, "subscribe"),
       RedisReply(RedisReplyType::kString, topic1), RedisReply(1)});
  tester->SyncClient();

  if (ps0) {
    tester->UseClient(ps0);
    tester->DoRedisTestArray(__LINE__, {"pubsub", "channels"}, {topic1});
    tester->DoRedisTestResultsArray(
        __LINE__, {"pubsub", "numsub", topic1, topic2},
        {RedisReply(RedisReplyType::kString, topic1), RedisReply(1),
         RedisReply(RedisReplyType::kString, topic2), RedisReply(0)});
    tester->DoRedisTestInt(__LINE__, {"pubsub", "numpat"}, 0);
    tester->SyncClient();
  }

  tester->UseClient(sc2);
  tester->DoRedisTestResultsArray(
      __LINE__, {"SUBSCRIBE", topic2},
      {RedisReply(RedisReplyType::kString, "subscribe"),
       RedisReply(RedisReplyType::kString, topic2), RedisReply(1)});
  tester->SyncClient();

  if (ps0) {
    tester->UseClient(ps0);
    tester->DoRedisTestArray(__LINE__, {"pubsub", "channels"}, {topic1, topic2});
    tester->DoRedisTestResultsArray(
        __LINE__, {"pubsub", "numsub", topic1, topic2},
        {RedisReply(RedisReplyType::kString, topic1), RedisReply(1),
         RedisReply(RedisReplyType::kString, topic2), RedisReply(1)});
    tester->DoRedisTestInt(__LINE__, {"pubsub", "numpat"}, 0);
    tester->SyncClient();
  }

  tester->UseClient(sc3);
  tester->DoRedisTestResultsArray(
      __LINE__, {"SUBSCRIBE", topic1},
      {RedisReply(RedisReplyType::kString, "subscribe"),
       RedisReply(RedisReplyType::kString, topic1), RedisReply(1)});
  tester->DoRedisTestResultsArray(
      __LINE__, {"SUBSCRIBE", topic2},
      {RedisReply(RedisReplyType::kString, "subscribe"),
       RedisReply(RedisReplyType::kString, topic2), RedisReply(2)});
  tester->SyncClient();

  if (ps0) {
    tester->UseClient(ps0);
    tester->DoRedisTestArray(__LINE__, {"pubsub", "channels"}, {topic1, topic2});
    tester->DoRedisTestResultsArray(
        __LINE__, {"pubsub", "numsub", topic1, topic2},
        {RedisReply(RedisReplyType::kString, topic1), RedisReply(2),
         RedisReply(RedisReplyType::kString, topic2), RedisReply(2)});
    tester->DoRedisTestInt(__LINE__, {"pubsub", "numpat"}, 0);
    tester->SyncClient();
  }

  // Now send msg1 to topic 1.
  tester->UseClient(pc1);
  tester->DoRedisTestInt(__LINE__, {"PUBLISH", topic1, msg1}, 2);
  tester->SyncClient();

  // Now send msg2 to topic 2.
  tester->UseClient(pc2);
  tester->DoRedisTestInt(__LINE__, {"PUBLISH", topic2, msg2}, 2);
  tester->SyncClient();

  // Verify the received messages.
  tester->UseClient(sc1);
  tester->DoRedisTestArray(__LINE__, {}, {"message", topic1, msg1});
  tester->SyncClient();

  tester->UseClient(sc2);
  tester->DoRedisTestArray(__LINE__, {}, {"message", topic2, msg2});
  // No more messages to receive.
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();

  tester->UseClient(sc3);
  tester->DoRedisTestArray(__LINE__, {}, {"message", topic1, msg1});
  tester->DoRedisTestArray(__LINE__, {}, {"message", topic2, msg2});
  // No more messages to receive.
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();

  tester->UseClient(nullptr);
  tester->VerifyCallbacks();
}

void TestUnsubscribe(
    TestRedisService* tester,
    std::shared_ptr<RedisClient> ps0,  // Used for PubSub command
    std::shared_ptr<RedisClient> sc1,  // Used for Subscribe
    std::shared_ptr<RedisClient> sc2,
    std::shared_ptr<RedisClient> sc3,
    std::shared_ptr<RedisClient> pc1,  // Used for Publish
    std::shared_ptr<RedisClient> pc2) {
  const string topic1 = "topic1", topic2 = "topic2";
  const string msg1 = "msg1", msg2 = "msg2", msg3 = "msg3", msg4 = "msg4";

  tester->UseClient(sc1);
  tester->DoRedisTestResultsArray(
      __LINE__, {"SUBSCRIBE", topic1, topic2},
      {RedisReply(RedisReplyType::kString, "subscribe"),
       RedisReply(RedisReplyType::kString, topic1), RedisReply(1)});
  tester->DoRedisTestResultsArray(
      __LINE__, {}, {RedisReply(RedisReplyType::kString, "subscribe"),
                     RedisReply(RedisReplyType::kString, topic2), RedisReply(2)});
  tester->SyncClient();

  tester->UseClient(sc2);
  tester->DoRedisTestResultsArray(
      __LINE__, {"SUBSCRIBE", topic1, topic2},
      {RedisReply(RedisReplyType::kString, "subscribe"),
       RedisReply(RedisReplyType::kString, topic1), RedisReply(1)});
  tester->DoRedisTestResultsArray(
      __LINE__, {}, {RedisReply(RedisReplyType::kString, "subscribe"),
                     RedisReply(RedisReplyType::kString, topic2), RedisReply(2)});
  tester->SyncClient();

  tester->UseClient(sc3);
  tester->DoRedisTestResultsArray(
      __LINE__, {"SUBSCRIBE", topic1, topic2},
      {RedisReply(RedisReplyType::kString, "subscribe"),
       RedisReply(RedisReplyType::kString, topic1), RedisReply(1)});
  tester->DoRedisTestResultsArray(
      __LINE__, {}, {RedisReply(RedisReplyType::kString, "subscribe"),
                     RedisReply(RedisReplyType::kString, topic2), RedisReply(2)});
  tester->SyncClient();

  if (ps0) {
    tester->UseClient(ps0);
    tester->DoRedisTestArray(__LINE__, {"pubsub", "channels"}, {topic1, topic2});
    tester->DoRedisTestResultsArray(
        __LINE__, {"pubsub", "numsub", topic1, topic2},
        {RedisReply(RedisReplyType::kString, topic1), RedisReply(3),
         RedisReply(RedisReplyType::kString, topic2), RedisReply(3)});
    tester->DoRedisTestInt(__LINE__, {"pubsub", "numpat"}, 0);
    tester->SyncClient();
  }

  // sc1 will unsubscribe from topic1. Will still be subscribed to topic2.
  tester->UseClient(sc1);
  tester->DoRedisTestResultsArray(
      __LINE__, {"UNSUBSCRIBE", topic1},
      {RedisReply(RedisReplyType::kString, "unsubscribe"),
       RedisReply(RedisReplyType::kString, topic1), RedisReply(1)});
  tester->SyncClient();

  // sc2 will unsubscribe from all topics. Will still be subscribed to none.
  tester->UseClient(sc2);
  tester->DoRedisTestResultsArray(
      __LINE__, {"UNSUBSCRIBE", topic1, topic2},
      {RedisReply(RedisReplyType::kString, "unsubscribe"),
       RedisReply(RedisReplyType::kString, topic1), RedisReply(1)});
  tester->DoRedisTestResultsArray(
      __LINE__, {}, {RedisReply(RedisReplyType::kString, "unsubscribe"),
                     RedisReply(RedisReplyType::kString, topic2), RedisReply(0)});
  tester->SyncClient();

  if (ps0) {
    tester->UseClient(ps0);
    tester->DoRedisTestArray(__LINE__, {"pubsub", "channels"}, {topic1, topic2});
    tester->DoRedisTestResultsArray(
        __LINE__, {"pubsub", "numsub", topic1, topic2},
        {RedisReply(RedisReplyType::kString, topic1), RedisReply(1),
         RedisReply(RedisReplyType::kString, topic2), RedisReply(2)});
    tester->DoRedisTestInt(__LINE__, {"pubsub", "numpat"}, 0);
    tester->SyncClient();
  }

  // Now send msg1 to topic 1.
  tester->UseClient(pc1);
  tester->DoRedisTestInt(__LINE__, {"PUBLISH", topic1, msg1}, 1);
  tester->SyncClient();

  // Now send msg2 to topic 2.
  tester->UseClient(pc2);
  tester->DoRedisTestInt(__LINE__, {"PUBLISH", topic2, msg2}, 2);
  tester->SyncClient();

  // Verify the received messages.
  tester->UseClient(sc1);
  tester->DoRedisTestArray(__LINE__, {}, {"message", topic2, msg2});
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();

  tester->UseClient(sc2);
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();

  tester->UseClient(sc3);
  tester->DoRedisTestArray(__LINE__, {}, {"message", topic1, msg1});
  tester->DoRedisTestArray(__LINE__, {}, {"message", topic2, msg2});
  // No more messages to receive.
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();

  // sc3 will unsubscribe from all topics. sc1 will still be subscribed to topic2.
  tester->UseClient(sc3);
  // Redis does not specify a particular order. So, the following two messages
  // could be in received in either order.
  tester->DoRedisTestResultsArray(
      __LINE__, {"UNSUBSCRIBE"}, {RedisReply(RedisReplyType::kString, "unsubscribe"),
                                  RedisReply(RedisReplyType::kString, "IGNORED"), RedisReply(1)});
  tester->DoRedisTestResultsArray(
      __LINE__, {}, {RedisReply(RedisReplyType::kString, "unsubscribe"),
                     RedisReply(RedisReplyType::kString, "IGNORED"), RedisReply(0)});
  tester->SyncClient();

  if (ps0) {
    tester->UseClient(ps0);
    tester->DoRedisTestArray(__LINE__, {"pubsub", "channels"}, {topic2});
    tester->DoRedisTestResultsArray(
        __LINE__, {"pubsub", "numsub", topic1, topic2},
        {RedisReply(RedisReplyType::kString, topic1), RedisReply(0),
         RedisReply(RedisReplyType::kString, topic2), RedisReply(1)});
    tester->DoRedisTestInt(__LINE__, {"pubsub", "numpat"}, 0);
    tester->SyncClient();
  }

  // Now send msg3 to topic 2.
  tester->UseClient(pc2);
  tester->DoRedisTestInt(__LINE__, {"PUBLISH", topic2, msg3}, 1);
  tester->SyncClient();

  // No one should receive the message except sc1.
  tester->UseClient(sc1);
  tester->DoRedisTestArray(__LINE__, {}, {"message", topic2, msg3});
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();
  tester->UseClient(sc2);
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();
  tester->UseClient(sc3);
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();

  // sc1 will unsubscribe from topic2. No one left subscribed to any topic.
  tester->UseClient(sc1);
  tester->DoRedisTestResultsArray(
      __LINE__, {"UNSUBSCRIBE"}, {RedisReply(RedisReplyType::kString, "unsubscribe"),
                                  RedisReply(RedisReplyType::kString, topic2), RedisReply(0)});
  tester->SyncClient();

  if (ps0) {
    tester->UseClient(ps0);
    tester->DoRedisTestArray(__LINE__, {"pubsub", "channels"}, {});
    tester->DoRedisTestResultsArray(
        __LINE__, {"pubsub", "numsub", topic1, topic2},
        {RedisReply(RedisReplyType::kString, topic1), RedisReply(0),
         RedisReply(RedisReplyType::kString, topic2), RedisReply(0)});
    tester->DoRedisTestInt(__LINE__, {"pubsub", "numpat"}, 0);
    tester->SyncClient();
  }

  // Now send msg4 to topic 2.
  tester->UseClient(pc2);
  tester->DoRedisTestInt(__LINE__, {"PUBLISH", topic2, msg4}, 0);
  tester->SyncClient();

  // No one should receive the message.
  tester->UseClient(sc1);
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();
  tester->UseClient(sc2);
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();
  tester->UseClient(sc3);
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();

  tester->UseClient(nullptr);
  tester->VerifyCallbacks();
}

void TestPSubscribe(
    TestRedisService* tester,
    std::shared_ptr<RedisClient> ps0,  // Used for PubSub command
    std::shared_ptr<RedisClient> sc1,  // Used for Subscribe
    std::shared_ptr<RedisClient> sc2,
    std::shared_ptr<RedisClient> sc3,
    std::shared_ptr<RedisClient> pc1,  // Used for Publish
    std::shared_ptr<RedisClient> pc2) {
  const string pattern1 = "t*1", pattern2 = "t*2", common_pattern = "t*";
  const string topic1 = "topic1", topic2 = "topic2";
  const string msg1 = "msg1", msg2 = "msg2";

  if (ps0) {
    tester->UseClient(ps0);
    tester->DoRedisTestArray(__LINE__, {"pubsub", "channels"}, {});
    tester->DoRedisTestResultsArray(
        __LINE__, {"pubsub", "numsub", topic1, topic2},
        {RedisReply(RedisReplyType::kString, topic1), RedisReply(0),
         RedisReply(RedisReplyType::kString, topic2), RedisReply(0)});
    tester->DoRedisTestInt(__LINE__, {"pubsub", "numpat"}, 0);
    tester->SyncClient();
  }

  tester->UseClient(sc1);
  tester->DoRedisTestResultsArray(
      __LINE__, {"PSUBSCRIBE", pattern1},
      {RedisReply(RedisReplyType::kString, "psubscribe"),
       RedisReply(RedisReplyType::kString, pattern1), RedisReply(1)});
  tester->SyncClient();

  if (ps0) {
    tester->UseClient(ps0);
    tester->DoRedisTestArray(__LINE__, {"pubsub", "channels"}, {});
    tester->DoRedisTestResultsArray(
        __LINE__, {"pubsub", "numsub", topic1, topic2},
        {RedisReply(RedisReplyType::kString, topic1), RedisReply(0),
         RedisReply(RedisReplyType::kString, topic2), RedisReply(0)});
    tester->DoRedisTestInt(__LINE__, {"pubsub", "numpat"}, 1);
    tester->SyncClient();
  }

  tester->UseClient(sc2);
  tester->DoRedisTestResultsArray(
      __LINE__, {"psubscribe", pattern2},
      {RedisReply(RedisReplyType::kString, "psubscribe"),
       RedisReply(RedisReplyType::kString, pattern2), RedisReply(1)});
  tester->SyncClient();

  if (ps0) {
    tester->UseClient(ps0);
    tester->DoRedisTestArray(__LINE__, {"pubsub", "channels"}, {});
    tester->DoRedisTestResultsArray(
        __LINE__, {"pubsub", "numsub", topic1, topic2},
        {RedisReply(RedisReplyType::kString, topic1), RedisReply(0),
         RedisReply(RedisReplyType::kString, topic2), RedisReply(0)});
    tester->DoRedisTestInt(__LINE__, {"pubsub", "numpat"}, 2);
    tester->SyncClient();
  }

  tester->UseClient(sc3);
  tester->DoRedisTestResultsArray(
      __LINE__, {"psubscribe", common_pattern},
      {RedisReply(RedisReplyType::kString, "psubscribe"),
       RedisReply(RedisReplyType::kString, common_pattern), RedisReply(1)});
  tester->SyncClient();

  // Now send msg1 to pattern 1.
  tester->UseClient(pc1);
  tester->DoRedisTestInt(__LINE__, {"PUBLISH", topic1, msg1}, 2);
  tester->SyncClient();

  // Now send msg2 to pattern 2.
  tester->UseClient(pc2);
  tester->DoRedisTestInt(__LINE__, {"PUBLISH", topic2, msg2}, 2);
  tester->SyncClient();

  // Verify the received messages.
  tester->UseClient(sc1);
  tester->DoRedisTestArray(__LINE__, {}, {"pmessage", pattern1, topic1, msg1});
  tester->SyncClient();

  tester->UseClient(sc2);
  tester->DoRedisTestArray(__LINE__, {}, {"pmessage", pattern2, topic2, msg2});
  // No more messages to receive.
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();

  tester->UseClient(sc3);
  tester->DoRedisTestArray(__LINE__, {}, {"pmessage", common_pattern, topic1, msg1});
  tester->DoRedisTestArray(__LINE__, {}, {"pmessage", common_pattern, topic2, msg2});
  // No more messages to receive.
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();

  tester->UseClient(nullptr);
  tester->VerifyCallbacks();
}

void TestPUnsubscribe(
    TestRedisService* tester,
    std::shared_ptr<RedisClient> ps0,  // Used for PubSub command
    std::shared_ptr<RedisClient> sc1,  // Used for Subscribe
    std::shared_ptr<RedisClient> sc2,
    std::shared_ptr<RedisClient> sc3,
    std::shared_ptr<RedisClient> pc1,  // Used for Publish
    std::shared_ptr<RedisClient> pc2) {
  const string pattern1 = "to*1", pattern2 = "to*2";
  const string topic1 = "topic1", topic2 = "topic2";
  const string msg1 = "msg1", msg2 = "msg2", msg3 = "msg3";

  if (ps0) {
    tester->UseClient(ps0);
    tester->DoRedisTestArray(__LINE__, {"pubsub", "channels"}, {});
    tester->DoRedisTestResultsArray(
        __LINE__, {"pubsub", "numsub", topic1, topic2},
        {RedisReply(RedisReplyType::kString, topic1), RedisReply(0),
         RedisReply(RedisReplyType::kString, topic2), RedisReply(0)});
    tester->DoRedisTestInt(__LINE__, {"pubsub", "numpat"}, 0);
    tester->SyncClient();
  }

  tester->UseClient(sc1);
  tester->DoRedisTestResultsArray(
      __LINE__, {"psubscribe", pattern1, pattern2},
      {RedisReply(RedisReplyType::kString, "psubscribe"),
       RedisReply(RedisReplyType::kString, pattern1), RedisReply(1)});
  tester->DoRedisTestResultsArray(
      __LINE__, {}, {RedisReply(RedisReplyType::kString, "psubscribe"),
                     RedisReply(RedisReplyType::kString, pattern2), RedisReply(2)});
  tester->SyncClient();

  if (ps0) {
    tester->UseClient(ps0);
    tester->DoRedisTestArray(__LINE__, {"pubsub", "channels"}, {});
    tester->DoRedisTestResultsArray(
        __LINE__, {"pubsub", "numsub", topic1, topic2},
        {RedisReply(RedisReplyType::kString, topic1), RedisReply(0),
         RedisReply(RedisReplyType::kString, topic2), RedisReply(0)});
    tester->DoRedisTestInt(__LINE__, {"pubsub", "numpat"}, 2);
    tester->SyncClient();
  }

  tester->UseClient(sc2);
  tester->DoRedisTestResultsArray(
      __LINE__, {"psubscribe", pattern1, pattern2},
      {RedisReply(RedisReplyType::kString, "psubscribe"),
       RedisReply(RedisReplyType::kString, pattern1), RedisReply(1)});
  tester->DoRedisTestResultsArray(
      __LINE__, {}, {RedisReply(RedisReplyType::kString, "psubscribe"),
                     RedisReply(RedisReplyType::kString, pattern2), RedisReply(2)});
  tester->SyncClient();

  if (ps0) {
    tester->UseClient(ps0);
    tester->DoRedisTestArray(__LINE__, {"pubsub", "channels"}, {});
    tester->DoRedisTestResultsArray(
        __LINE__, {"pubsub", "numsub", topic1, topic2},
        {RedisReply(RedisReplyType::kString, topic1), RedisReply(0),
         RedisReply(RedisReplyType::kString, topic2), RedisReply(0)});
    tester->DoRedisTestInt(__LINE__, {"pubsub", "numpat"}, 2);
    tester->SyncClient();
  }

  tester->UseClient(sc3);
  tester->DoRedisTestResultsArray(
      __LINE__, {"psubscribe", pattern1, pattern2},
      {RedisReply(RedisReplyType::kString, "psubscribe"),
       RedisReply(RedisReplyType::kString, pattern1), RedisReply(1)});
  tester->DoRedisTestResultsArray(
      __LINE__, {}, {RedisReply(RedisReplyType::kString, "psubscribe"),
                     RedisReply(RedisReplyType::kString, pattern2), RedisReply(2)});
  tester->SyncClient();

  if (ps0) {
    tester->UseClient(ps0);
    tester->DoRedisTestArray(__LINE__, {"pubsub", "channels"}, {});
    tester->DoRedisTestResultsArray(
        __LINE__, {"pubsub", "numsub", topic1, topic2},
        {RedisReply(RedisReplyType::kString, topic1), RedisReply(0),
         RedisReply(RedisReplyType::kString, topic2), RedisReply(0)});
    tester->DoRedisTestInt(__LINE__, {"pubsub", "numpat"}, 2);
    tester->SyncClient();
  }

  // sc1 will punsubscribe from pattern2. Will still be psubscribed to pattern1.
  tester->UseClient(sc1);
  tester->DoRedisTestResultsArray(
      __LINE__, {"punsubscribe", pattern1},
      {RedisReply(RedisReplyType::kString, "punsubscribe"),
       RedisReply(RedisReplyType::kString, pattern1), RedisReply(1)});
  tester->SyncClient();

  // sc2 will punsubscribe from all patterns. Will still be psubscribed to none.
  tester->UseClient(sc2);
  tester->DoRedisTestResultsArray(
      __LINE__, {"punsubscribe", pattern1, pattern2},
      {RedisReply(RedisReplyType::kString, "punsubscribe"),
       RedisReply(RedisReplyType::kString, pattern1), RedisReply(1)});
  tester->DoRedisTestResultsArray(
      __LINE__, {}, {RedisReply(RedisReplyType::kString, "punsubscribe"),
                     RedisReply(RedisReplyType::kString, pattern2), RedisReply(0)});
  tester->SyncClient();

  // Now send msg1 to pattern 1.
  tester->UseClient(pc1);
  tester->DoRedisTestInt(__LINE__, {"PUBLISH", topic1, msg1}, 1);
  tester->SyncClient();

  // Now send msg2 to pattern 2.
  tester->UseClient(pc2);
  tester->DoRedisTestInt(__LINE__, {"PUBLISH", topic2, msg2}, 2);
  tester->SyncClient();

  // Verify the received pmessages.
  tester->UseClient(sc1);
  tester->DoRedisTestArray(__LINE__, {}, {"pmessage", pattern2, topic2, msg2});
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();

  tester->UseClient(sc2);
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();

  tester->UseClient(sc3);
  tester->DoRedisTestArray(__LINE__, {}, {"pmessage", pattern1, topic1, msg1});
  tester->DoRedisTestArray(__LINE__, {}, {"pmessage", pattern2, topic2, msg2});
  // No more messages to receive.
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();

  if (ps0) {
    tester->UseClient(ps0);
    tester->DoRedisTestArray(__LINE__, {"pubsub", "channels"}, {});
    tester->DoRedisTestResultsArray(
        __LINE__, {"pubsub", "numsub", topic1, topic2},
        {RedisReply(RedisReplyType::kString, topic1), RedisReply(0),
         RedisReply(RedisReplyType::kString, topic2), RedisReply(0)});
    tester->DoRedisTestInt(__LINE__, {"pubsub", "numpat"}, 2);
    tester->SyncClient();
  }

  // sc3 will punsubscribe from all patterns.
  tester->UseClient(sc3);
  // Redis does not specify a particular order. So, the following two messages
  // could be in received in either order.
  tester->DoRedisTestResultsArray(
      __LINE__, {"punsubscribe"}, {RedisReply(RedisReplyType::kString, "punsubscribe"),
                                   RedisReply(RedisReplyType::kString, "IGNORED"), RedisReply(1)});
  tester->DoRedisTestResultsArray(
      __LINE__, {}, {RedisReply(RedisReplyType::kString, "punsubscribe"),
                     RedisReply(RedisReplyType::kString, "IGNORED"), RedisReply(0)});
  tester->SyncClient();

  if (ps0) {
    tester->UseClient(ps0);
    tester->DoRedisTestArray(__LINE__, {"pubsub", "channels"}, {});
    tester->DoRedisTestResultsArray(
        __LINE__, {"pubsub", "numsub", topic1, topic2},
        {RedisReply(RedisReplyType::kString, topic1), RedisReply(0),
         RedisReply(RedisReplyType::kString, topic2), RedisReply(0)});
    tester->DoRedisTestInt(__LINE__, {"pubsub", "numpat"}, 1);
    tester->SyncClient();
  }

  // Now send msg3 to pattern 2. only sc1 receives it.
  tester->UseClient(pc2);
  tester->DoRedisTestInt(__LINE__, {"PUBLISH", topic2, msg3}, 1);
  tester->SyncClient();

  // No one should receive the message.
  tester->UseClient(sc1);
  tester->DoRedisTestArray(__LINE__, {}, {"pmessage", pattern2, topic2, msg3});
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();
  tester->UseClient(sc2);
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();
  tester->UseClient(sc3);
  tester->DoRedisTestArray(__LINE__, {"PING"}, {"pong", ""});
  tester->SyncClient();

  // Get sc1 to also punsubscribe from pattern 2. No one is subscribed to any patterns anymore.
  tester->UseClient(sc1);
  tester->DoRedisTestResultsArray(
      __LINE__, {"punsubscribe"}, {RedisReply(RedisReplyType::kString, "punsubscribe"),
                                   RedisReply(RedisReplyType::kString, pattern2), RedisReply(0)});
  tester->SyncClient();

  if (ps0) {
    tester->UseClient(ps0);
    tester->DoRedisTestArray(__LINE__, {"pubsub", "channels"}, {});
    tester->DoRedisTestResultsArray(
        __LINE__, {"pubsub", "numsub", topic1, topic2},
        {RedisReply(RedisReplyType::kString, topic1), RedisReply(0),
         RedisReply(RedisReplyType::kString, topic2), RedisReply(0)});
    tester->DoRedisTestInt(__LINE__, {"pubsub", "numpat"}, 0);
    tester->SyncClient();
  }

  tester->UseClient(nullptr);
  tester->VerifyCallbacks();
}

// Utility for testing various combination(s).
YB_DEFINE_ENUM(SubOrUnsub, (kSubscribe)(kUnsubscribe));
YB_DEFINE_ENUM(PatternOrChannel, (kChannel)(kPattern));
YB_DEFINE_ENUM(LocalOrCluster, (kLocal)(kCluster));

class TestRedisServiceExternal : public TestRedisService {
 protected:
  void TestPubSub(LocalOrCluster ltype, SubOrUnsub stype, PatternOrChannel ptype);

  void CustomizeExternalMiniCluster(ExternalMiniClusterOptions* opts) override {
    opts->extra_tserver_flags.push_back(
        "--redis_connection_soft_limit_grace_period_sec=" +
        yb::ToString(kSoftLimitGracePeriod.ToSeconds()));
  }

  static const MonoDelta kSoftLimitGracePeriod;

 private:
  Endpoint RedisProxyEndpoint() override {
    auto ts0 = external_mini_cluster()->tablet_server(0);
    return Endpoint(IpAddress::from_string(ts0->bind_host()), ts0->redis_rpc_port());
  }

  bool use_external_mini_cluster() override { return true; }
};

const MonoDelta TestRedisServiceExternal::kSoftLimitGracePeriod = yb::NonTsanVsTsan(1s, 10s);

void TestRedisServiceExternal::TestPubSub(
    LocalOrCluster ltype, SubOrUnsub stype, PatternOrChannel ptype) {
  std::shared_ptr<RedisClient> ps0, sc1, sc2, sc3, pc1, pc2;

  if (ltype == LocalOrCluster::kLocal) {
    auto ts0 = external_mini_cluster()->tablet_server(0);
    auto host0 = ts0->bind_host();
    auto port0 = ts0->redis_rpc_port();

    sc1 = std::make_shared<RedisClient>(host0, port0);
    sc2 = std::make_shared<RedisClient>(host0, port0);
    sc3 = std::make_shared<RedisClient>(host0, port0);
    pc1 = std::make_shared<RedisClient>(host0, port0);
    pc2 = std::make_shared<RedisClient>(host0, port0);
    ps0 = std::make_shared<RedisClient>(host0, port0);
  } else {
    auto ts0 = external_mini_cluster()->tablet_server(0);
    auto host0 = ts0->bind_host();
    auto port0 = ts0->redis_rpc_port();
    auto ts1 = external_mini_cluster()->tablet_server(1);
    auto host1 = ts1->bind_host();
    auto port1 = ts1->redis_rpc_port();

    sc1 = std::make_shared<RedisClient>(host0, port0);
    sc2 = std::make_shared<RedisClient>(host1, port1);
    sc3 = std::make_shared<RedisClient>(host0, port0);
    pc1 = std::make_shared<RedisClient>(host0, port0);
    pc2 = std::make_shared<RedisClient>(host1, port1);
    ps0 = nullptr;  // Diabled. PubSub monitoring only queries the local proxy.
  }

  if (stype == SubOrUnsub::kSubscribe) {
    if (ptype == PatternOrChannel::kChannel) {
      TestSubscribe(this, ps0, sc1, sc2, sc3, pc1, pc2);
    } else {
      TestPSubscribe(this, ps0, sc1, sc2, sc3, pc1, pc2);
    }
  } else {
    if (ptype == PatternOrChannel::kChannel) {
      TestUnsubscribe(this, ps0, sc1, sc2, sc3, pc1, pc2);
    } else {
      TestPUnsubscribe(this, ps0, sc1, sc2, sc3, pc1, pc2);
    }
  }
}

TEST_F(TestRedisServiceExternal, TestSubscribe) {
  expected_no_sessions_ = true;
  TestPubSub(LocalOrCluster::kLocal, SubOrUnsub::kSubscribe, PatternOrChannel::kChannel);
}

TEST_F(TestRedisServiceExternal, TestSubscribeCluster) {
  expected_no_sessions_ = true;
  TestPubSub(LocalOrCluster::kCluster, SubOrUnsub::kSubscribe, PatternOrChannel::kChannel);
}

TEST_F(TestRedisServiceExternal, TestUnsubscribe) {
  expected_no_sessions_ = true;
  TestPubSub(LocalOrCluster::kLocal, SubOrUnsub::kUnsubscribe, PatternOrChannel::kChannel);
}

TEST_F(TestRedisServiceExternal, TestUnsubscribeCluster) {
  expected_no_sessions_ = true;
  TestPubSub(LocalOrCluster::kCluster, SubOrUnsub::kUnsubscribe, PatternOrChannel::kChannel);
}

TEST_F(TestRedisServiceExternal, TestPSubscribe) {
  expected_no_sessions_ = true;
  TestPubSub(LocalOrCluster::kLocal, SubOrUnsub::kSubscribe, PatternOrChannel::kPattern);
}

TEST_F(TestRedisServiceExternal, TestPSubscribeCluster) {
  expected_no_sessions_ = true;
  TestPubSub(LocalOrCluster::kCluster, SubOrUnsub::kSubscribe, PatternOrChannel::kPattern);
}

TEST_F(TestRedisServiceExternal, TestPUnsubscribe) {
  expected_no_sessions_ = true;
  TestPubSub(LocalOrCluster::kLocal, SubOrUnsub::kUnsubscribe, PatternOrChannel::kPattern);
}

TEST_F(TestRedisServiceExternal, TestPUnsubscribeCluster) {
  expected_no_sessions_ = true;
  TestPubSub(LocalOrCluster::kCluster, SubOrUnsub::kUnsubscribe, PatternOrChannel::kPattern);
}

TEST_F(TestRedisServiceExternal, TestSlowSubscribersCatchingUp) {
  expected_no_sessions_ = true;

  auto ts0 = external_mini_cluster()->tablet_server(0);
  auto host0 = ts0->bind_host();
  auto port0 = ts0->redis_rpc_port();

  auto sc1 = std::make_shared<RedisClient>(host0, port0);
  auto pc1 = std::make_shared<RedisClient>(host0, port0);

  const string topic1 = "topic1";
  const string padding(1_MB, 'x');

  UseClient(sc1);
  DoRedisTestResultsArray(
      __LINE__, {"SUBSCRIBE", topic1},
      {RedisReply(RedisReplyType::kString, "subscribe"),
       RedisReply(RedisReplyType::kString, topic1), RedisReply(1)});
  SyncClient();

  constexpr int kNumLoops = 3;
  constexpr int kNumMsgs = 20;
  const auto kSoftLimitGracePeriodMinusDelta =
      MonoDelta::FromSeconds(kSoftLimitGracePeriod.ToSeconds() * 0.8);
  const auto kSoftLimitGracePeriodPlusDelta =
      MonoDelta::FromSeconds(kSoftLimitGracePeriod.ToSeconds() * 1.2);
  for (int loops = 0; loops < kNumLoops; loops++) {
    // Write approx 20MB of data. More than the soft limit. But less than the hard limit.
    UseClient(pc1);
    for (int i = 0; i < kNumMsgs; i++) {
      // Now send msg1 to topic 1.
      auto msg = Substitute("trial-$0 : $1", i, padding);
      VLOG(2) << "Trial " << i << ". Publishing message of size " << msg.length() << " bytes";
      DoRedisTestInt(__LINE__, {"PUBLISH", topic1, msg}, 1);
      ASSERT_NO_FATALS(SyncClient());
    }

    // Wait for less than what the server to enforce the soft limit.
    SleepFor(kSoftLimitGracePeriodMinusDelta);

    for (int i = 0; i < kNumMsgs; i++) {
      // Verify the received messages.
      VLOG(2) << "Trial " << i << ". Receiving subscribed message";
      UseClient(sc1);
      auto msg = Substitute("trial-$0 : $1", i, padding);
      DoRedisTestArray(__LINE__, {}, {"message", topic1, msg});
      ASSERT_NO_FATALS(SyncClient());
    }

    SleepFor(kSoftLimitGracePeriodPlusDelta);
  }

  const string big_padding(20_MB, 'x');
  for (int loops = 0; loops < kNumLoops; loops++) {
    // Write > soft limit sized data in one shot.
    UseClient(pc1);
    auto msg = Substitute("Big-$0", big_padding);
    VLOG(2) << loops << ". Publishing a big message of size " << msg.length();
    DoRedisTestInt(__LINE__, {"PUBLISH", topic1, msg}, 1);
    ASSERT_NO_FATALS(SyncClient());

    // Wait for less than what the server to enforce the soft limit.
    SleepFor(kSoftLimitGracePeriodMinusDelta);

    UseClient(sc1);
    DoRedisTestArray(__LINE__, {}, {"message", topic1, msg});
    ASSERT_NO_FATALS(SyncClient());

    SleepFor(kSoftLimitGracePeriodPlusDelta);
  }
}

TEST_F(TestRedisServiceExternal, TestSlowSubscribersSoftLimit) {
  expected_no_sessions_ = true;

  auto ts0 = external_mini_cluster()->tablet_server(0);
  auto host0 = ts0->bind_host();
  auto port0 = ts0->redis_rpc_port();

  auto sc1 = std::make_shared<RedisClient>(host0, port0);
  auto pc1 = std::make_shared<RedisClient>(host0, port0);

  const string topic1 = "topic1";
  const string padding(1_MB, 'x');

  UseClient(sc1);
  DoRedisTestResultsArray(
      __LINE__, {"SUBSCRIBE", topic1},
      {RedisReply(RedisReplyType::kString, "subscribe"),
       RedisReply(RedisReplyType::kString, topic1), RedisReply(1)});
  SyncClient();

  UseClient(pc1);
  // Write approx 15MB of data. Something more than the soft limit.
  for (int i = 0; i < 15; i++) {
    // Now send msg1 to topic 1.
    auto msg = Substitute("trial-$0 : $1", i, padding);
    VLOG(2) << "Trial " << i << ". Publishing message of size " << msg.length() << " bytes";
    DoRedisTestInt(__LINE__, {"PUBLISH", topic1, msg}, 1);
    ASSERT_NO_FATALS(SyncClient());
  }

  // Wait for the server to enforce the soft limit.
  SleepFor(kSoftLimitGracePeriod);

  DoRedisTestApproxInt(__LINE__, {"PUBLISH", topic1, "whatever"}, 1, 1);
  ASSERT_NO_FATALS(SyncClient());
  for (int i = 15; i < 30; i++) {
    // Now send msg1 to topic 1.
    auto msg = Substitute("trial-$0 : $1", i, padding);
    VLOG(2) << "Trial " << i << ". Publishing message of size " << msg.length() << " bytes";
    DoRedisTestInt(__LINE__, {"PUBLISH", topic1, msg}, 0);
    ASSERT_NO_FATALS(SyncClient());
  }

  // sc1 should have already been disconnected.
  UseClient(sc1);
  DoRedisTestExpectError(__LINE__, {});
}

TEST_F(TestRedisServiceExternal, TestSlowSubscribersHardLimit) {
  expected_no_sessions_ = true;

  auto ts0 = external_mini_cluster()->tablet_server(0);
  auto host0 = ts0->bind_host();
  auto port0 = ts0->redis_rpc_port();

  auto sc1 = std::make_shared<RedisClient>(host0, port0);
  auto pc1 = std::make_shared<RedisClient>(host0, port0);

  const string topic1 = "topic1";
  const string padding(1_MB, 'x');

  UseClient(sc1);
  DoRedisTestResultsArray(
      __LINE__, {"SUBSCRIBE", topic1},
      {RedisReply(RedisReplyType::kString, "subscribe"),
       RedisReply(RedisReplyType::kString, topic1), RedisReply(1)});
  SyncClient();

  UseClient(pc1);
  // Write approx 32MB of data.
  for (int i = 0; i < 32; i++) {
    // Now send msg1 to topic 1.
    auto msg = Substitute("trial-$0 : $1", i, padding);
    VLOG(2) << "Trial " << i << ". Publishing message of size " << msg.length() << " bytes";
    DoRedisTestInt(__LINE__, {"PUBLISH", topic1, msg}, 1);
    ASSERT_NO_FATALS(SyncClient());
  }

  // Let's allow for some msgs to be either sent to the subscriber or unsent, to account for
  // buffering in the lower layers.
  for (int i = 32; i < 40; i++) {
    auto msg = Substitute("trial-$0 : $1", i, padding);
    VLOG(2) << "Trial " << i << ". Publishing message of size " << msg.length() << " bytes";
    DoRedisTestApproxInt(__LINE__, {"PUBLISH", topic1, msg}, 1, 1);
    ASSERT_NO_FATALS(SyncClient());
  }

  // The slow subscriber should have been disconnected. Expect the msg to be sent to no one.
  for (int i = 40; i < 50; i++) {
    // Now send msg1 to topic 1.
    auto msg = Substitute("trial-$0 : $1", i, padding);
    VLOG(2) << "Trial " << i << ". Publishing message of size " << msg.length() << " bytes";
    DoRedisTestInt(__LINE__, {"PUBLISH", topic1, msg}, 0);
    ASSERT_NO_FATALS(SyncClient());
  }

  UseClient(sc1);
  // sc1 should have already been disconnected.
  DoRedisTestExpectError(__LINE__, {});
}

TEST_F(TestRedisServiceExternal, SubscribedClientMode) {
  expected_no_sessions_ = true;
  const string topic1 = "topic1";
  const string value = "value";

  DoRedisTestSimpleString(__LINE__, {"PING"}, "PONG");
  DoRedisTestBulkString(__LINE__, {"PING", "cmd2"}, "cmd2");
  SyncClient();

  DoRedisTestResultsArray(
      __LINE__, {"SUBSCRIBE", topic1},
      {RedisReply(RedisReplyType::kString, "subscribe"),
       RedisReply(RedisReplyType::kString, topic1), RedisReply(1)});
  SyncClient();

  DoRedisTestExpectError(__LINE__, {"SET", "foo", value});
  SyncClient();

  DoRedisTestArray(__LINE__, {"PING", "cmd2"}, {"pong", "cmd2"});
  SyncClient();

  DoRedisTestOk(__LINE__, {"QUIT"});
  SyncClient();
}

TEST_F(TestRedisService, TestAuth) {
  FLAGS_redis_password_caching_duration_ms = 0;
  const char* kRedisAuthPassword = "redis-password";
  // Expect new connections to require authentication
  auto rc1 = std::make_shared<RedisClient>("127.0.0.1", server_port());
  auto rc2 = std::make_shared<RedisClient>("127.0.0.1", server_port());
  UseClient(rc1);
  DoRedisTestSimpleString(__LINE__, {"PING"}, "PONG");
  SyncClient();
  UseClient(rc2);
  DoRedisTestSimpleString(__LINE__, {"PING"}, "PONG");
  SyncClient();

  // Set require pass using one connection
  UseClient(rc1);
  DoRedisTestOk(__LINE__, {"CONFIG", "SET", "REQUIREPASS", kRedisAuthPassword});
  DoRedisTestArray(__LINE__, {"CONFIG", "GET", "REQUIREPASS"}, {});
  DoRedisTestArray(__LINE__, {"CONFIG", "GET", "FooBar"}, {});
  SyncClient();
  UseClient(nullptr);
  // Other pre-established connections should still be able to work, without re-authentication.
  UseClient(rc2);
  DoRedisTestSimpleString(__LINE__, {"PING"}, "PONG");
  SyncClient();

  // Ensure that new connections need the correct password to authenticate.
  ConnectWithPassword(this, nullptr, false, false);
  ConnectWithPassword(this, "wrong-password", false, false);
  ConnectWithPassword(this, kRedisAuthPassword, true, true);

  // Set multiple passwords.
  UseClient(rc1);
  DoRedisTestOk(__LINE__, {"CONFIG", "SET", "REQUIREPASS", "passwordA,passwordB"});
  SyncClient();
  UseClient(nullptr);

  ConnectWithPassword(this, nullptr, false, false);
  ConnectWithPassword(this, "wrong-password", false, false);
  // Old password should no longer work.
  ConnectWithPassword(this, kRedisAuthPassword, false, false);
  ConnectWithPassword(this, "passwordA", true, true);
  ConnectWithPassword(this, "passwordB", true, true);
  ConnectWithPassword(this, "passwordC", false, false);
  // Need to provide one. Not both while authenticating.
  ConnectWithPassword(this, "passwordA,passwordB", false, false);

  // Setting more than 2 passwords should fail.
  UseClient(rc1);
  DoRedisTestExpectError(
      __LINE__, {"CONFIG", "SET", "REQUIREPASS", "passwordA,passwordB,passwordC"});
  SyncClient();

  // Now set no password.
  DoRedisTestOk(__LINE__, {"CONFIG", "SET", "REQUIREPASS", ""});
  SyncClient();
  UseClient(nullptr);

  // Setting wrong/old password(s) should fail. But set/get commands after that should succeed
  // regardless.
  ConnectWithPassword(this, "wrong-password", false, true);
  ConnectWithPassword(this, kRedisAuthPassword, false, true);
  ConnectWithPassword(this, "passwordA", false, true);
  ConnectWithPassword(this, "passwordB", false, true);
  ConnectWithPassword(this, nullptr, true, true);

  VerifyCallbacks();
}

TEST_F(TestRedisService, TestPasswordChangeWithDelay) {
  constexpr uint32 kCachingDurationMs = 1000;
  FLAGS_redis_password_caching_duration_ms = kCachingDurationMs;
  const char* kRedisAuthPassword = "redis-password";
  auto start = std::chrono::steady_clock::now();
  auto rc1 = std::make_shared<RedisClient>("127.0.0.1", server_port());

  UseClient(rc1);
  DoRedisTestOk(__LINE__, {"CONFIG", "SET", "REQUIREPASS", kRedisAuthPassword});
  SyncClient();
  UseClient(nullptr);

  // Proxy may not realize the password change immediately.
  // Expect the old password to work only if we haven't taken too long to get here.
  const std::chrono::milliseconds kNotTooLong(kCachingDurationMs / 2);
  auto now = std::chrono::steady_clock::now();
  if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start) < kNotTooLong) {
    ConnectWithPassword(this, nullptr, true, true);
    ConnectWithPassword(this, kRedisAuthPassword, false, true);
  }

  // Wait for the cached redis credentials in the redis proxy to expire.
  constexpr uint32 kDelayMs = 100;
  std::this_thread::sleep_for(std::chrono::milliseconds(kCachingDurationMs + kDelayMs));

  // Expect the proxy to realize the effect of the password change.
  ConnectWithPassword(this, nullptr, false, false);
  ConnectWithPassword(this, kRedisAuthPassword, true, true);

  VerifyCallbacks();
}

TEST_F(TestRedisService, TestRename) {
  DoRedisTestOk(__LINE__, {"SET", "k1", "5"});
  SyncClient();
  DoRedisTestInt(__LINE__, {"EXPIRE", "k1", "100"}, 1);
  SyncClient();

  DoRedisTestBulkString(__LINE__, {"GET", "k1"}, "5");
  DoRedisTestNull(__LINE__, {"GET", "k2"});
  SyncClient();
  DoRedisTestApproxInt(__LINE__, {"TTL", "k1"}, 100, 5);
  DoRedisTestInt(__LINE__, {"TTL", "k2"}, -2);
  SyncClient();

  DoRedisTestOk(__LINE__, {"RENAME", "k1", "k2"});
  SyncClient();

  DoRedisTestNull(__LINE__, {"GET", "k1"});
  DoRedisTestBulkString(__LINE__, {"GET", "k2"}, "5");
  SyncClient();
  DoRedisTestApproxInt(__LINE__, {"TTL", "k2"}, 100, 5);
  DoRedisTestInt(__LINE__, {"TTL", "k1"}, -2);
  SyncClient();

  // Degenerate case src == dest.
  DoRedisTestOk(__LINE__, {"RENAME", "k2", "k2"});
  SyncClient();
  DoRedisTestBulkString(__LINE__, {"GET", "k2"}, "5");
  SyncClient();

  // Failure cases.
  DoRedisTestExpectError(__LINE__, {"RENAME", "non-existent", "k2"});
  SyncClient();
  DoRedisTestBulkString(__LINE__, {"GET", "k2"}, "5");
  SyncClient();
}

TEST_F(TestRedisService, TestRenameSameTablet) {
  // Rename to a key in the same tablet
  // specify prefix to ensure that the keys are on the same tablet.
  DoRedisTestOk(__LINE__, {"SET", "{k}0", "5"});
  DoRedisTestOk(__LINE__, {"RENAME", "{k}0", "{k}xxxxxx"});
  SyncClient();
  DoRedisTestNull(__LINE__, {"GET", "{k}0"});
  DoRedisTestBulkString(__LINE__, {"GET", "{k}xxxxxx"}, "5");
  SyncClient();
}

TEST_F(TestRedisService, TestRenameSameTabletRandomized) {
  // Rename to a key in the same tablet
  // randomized 1/24 odds of being in the same tablet as k0.
  for (int i = 1; i < 100; i++) {
    const string dest = strings::Substitute("k$0", i);
    VLOG(1) << "Renaming from k0 to " << dest;
    DoRedisTestOk(__LINE__, {"SET", "k0", "5"});
    SyncClient();
    DoRedisTestOk(__LINE__, {"RENAME", "k0", dest});
    SyncClient();
    DoRedisTestNull(__LINE__, {"GET", "k0"});
    DoRedisTestBulkString(__LINE__, {"GET", dest}, "5");
    SyncClient();
  }
}

TEST_F(TestRedisService, TestRenamePipeline) {
  // Pipeline case.
  DoRedisTestOk(__LINE__, {"SET", "ka", "4"});
  SyncClient();

  DoRedisTestOk(__LINE__, {"SET", "ka", "5"});
  DoRedisTestOk(__LINE__, {"RENAME", "ka", "kb"});
  DoRedisTestBulkString(__LINE__, {"GET", "kb"}, "5");
  DoRedisTestNull(__LINE__, {"GET", "ka"});
  SyncClient();
}

TEST_F(TestRedisService, TestRenameHash) {
  DoRedisTestInt(__LINE__, {"HSET", "k1", "s1", "5"}, 1);
  DoRedisTestInt(__LINE__, {"HSET", "k1", "s2", "6"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"EXPIRE", "k1", "100"}, 1);

  SyncClient();
  DoRedisTestOk(__LINE__, {"SET", "k2", "x"});
  SyncClient();

  DoRedisTestBulkString(__LINE__, {"HGET", "k1", "s1"}, "5");
  DoRedisTestBulkString(__LINE__, {"HGET", "k1", "s2"}, "6");
  DoRedisTestBulkString(__LINE__, {"GET", "k2"}, "x");
  SyncClient();
  DoRedisTestApproxInt(__LINE__, {"TTL", "k1"}, 100, 5);
  DoRedisTestInt(__LINE__, {"TTL", "k2"}, -1);
  SyncClient();

  DoRedisTestOk(__LINE__, {"RENAME", "k1", "k2"});
  SyncClient();

  DoRedisTestBulkString(__LINE__, {"HGET", "k2", "s1"}, "5");
  DoRedisTestBulkString(__LINE__, {"HGET", "k2", "s2"}, "6");
  DoRedisTestNull(__LINE__, {"GET", "k1"});
  DoRedisTestNull(__LINE__, {"HGET", "k1", "s1"});
  SyncClient();
  DoRedisTestApproxInt(__LINE__, {"TTL", "k2"}, 100, 5);
  DoRedisTestInt(__LINE__, {"TTL", "k1"}, -2);
  SyncClient();
}

TEST_F(TestRedisService, TestRenameSet) {
  DoRedisTestInt(__LINE__, {"SADD", "k1", "s1", "s2"}, 2);
  SyncClient();
  DoRedisTestInt(__LINE__, {"EXPIRE", "k1", "100"}, 1);
  SyncClient();

  DoRedisTestArray(__LINE__, {"SMEMBERS", "k1"}, {"s1", "s2"});
  DoRedisTestNull(__LINE__, {"GET", "k2"});
  DoRedisTestArray(__LINE__, {"SMEMBERS", "k2"}, {});
  SyncClient();
  DoRedisTestApproxInt(__LINE__, {"TTL", "k1"}, 100, 5);
  DoRedisTestInt(__LINE__, {"TTL", "k2"}, -2);
  SyncClient();

  DoRedisTestOk(__LINE__, {"RENAME", "k1", "k2"});
  SyncClient();

  DoRedisTestArray(__LINE__, {"SMEMBERS", "k2"}, {"s1", "s2"});
  DoRedisTestNull(__LINE__, {"GET", "k1"});
  DoRedisTestArray(__LINE__, {"SMEMBERS", "k1"}, {});
  SyncClient();
  DoRedisTestApproxInt(__LINE__, {"TTL", "k2"}, 100, 5);
  DoRedisTestInt(__LINE__, {"TTL", "k1"}, -2);
  SyncClient();
}

TEST_F(TestRedisService, TestRenameSortedSet) {
  DoRedisTestInt(__LINE__, {"ZADD", "k1", "-2", "sk1", "2", "sk2"}, 2);
  SyncClient();
  DoRedisTestInt(__LINE__, {"EXPIRE", "k1", "100"}, 1);
  SyncClient();

  DoRedisTestScoreValueArray(
      __LINE__, {"ZRANGEBYSCORE", "k1", "-inf", "+inf", "WITHSCORES"}, {-2, 2}, {"sk1", "sk2"});
  DoRedisTestNull(__LINE__, {"GET", "k2"});
  SyncClient();
  DoRedisTestApproxInt(__LINE__, {"TTL", "k1"}, 100, 5);
  DoRedisTestInt(__LINE__, {"TTL", "k2"}, -2);
  SyncClient();

  DoRedisTestOk(__LINE__, {"RENAME", "k1", "k2"});
  SyncClient();

  DoRedisTestScoreValueArray(
      __LINE__, {"ZRANGEBYSCORE", "k2", "-inf", "+inf", "WITHSCORES"}, {-2, 2}, {"sk1", "sk2"});
  DoRedisTestNull(__LINE__, {"GET", "k1"});
  SyncClient();
  DoRedisTestApproxInt(__LINE__, {"TTL", "k2"}, 100, 5);
  DoRedisTestInt(__LINE__, {"TTL", "k1"}, -2);
  SyncClient();
}

TEST_F(TestRedisService, TestRenameTSType) {
  DoRedisTestOk(__LINE__, {"TSADD", "k1", "2", "sk1", "-2", "sk2"});
  SyncClient();

  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "k1", "-inf", "+inf"}, {"-2", "sk2", "2", "sk1"});
  DoRedisTestNull(__LINE__, {"GET", "k2"});
  SyncClient();

  DoRedisTestOk(__LINE__, {"RENAME", "k1", "k2"});
  SyncClient();

  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "k2", "-inf", "+inf"}, {"-2", "sk2", "2", "sk1"});
  DoRedisTestNull(__LINE__, {"GET", "k1"});
  SyncClient();
}

TEST_F(TestRedisService, TestIncr) {
  DoRedisTestOk(__LINE__, {"SET", "k1", "5"});
  DoRedisTestBulkString(__LINE__, {"GET", "k1"}, "5");

  DoRedisTestInt(__LINE__, {"INCR", "k1"}, 6);
  DoRedisTestBulkString(__LINE__, {"GET", "k1"}, "6");

  DoRedisTestInt(__LINE__, {"INCRBY", "k1", "4"}, 10);
  DoRedisTestBulkString(__LINE__, {"GET", "k1"}, "10");

  DoRedisTestInt(__LINE__, {"INCRBY", "k1", "-5"}, 5);
  DoRedisTestBulkString(__LINE__, {"GET", "k1"}, "5");

  DoRedisTestNull(__LINE__, {"GET", "kne1"});
  DoRedisTestInt(__LINE__, {"INCR", "kne1"}, 1);
  DoRedisTestBulkString(__LINE__, {"GET", "kne1"}, "1");

  DoRedisTestNull(__LINE__, {"GET", "kne2"});
  DoRedisTestInt(__LINE__, {"INCRBY", "kne2", "5"}, 5);
  DoRedisTestBulkString(__LINE__, {"GET", "kne2"}, "5");
  SyncClient();

  DoRedisTestInt(__LINE__, {"HSET", "h1", "f1", "5"}, 1);
  DoRedisTestBulkString(__LINE__, {"HGET", "h1", "f1"}, "5");
  SyncClient();

  DoRedisTestInt(__LINE__, {"HINCRBY", "h1", "f1", "1"}, 6);
  DoRedisTestBulkString(__LINE__, {"HGET", "h1", "f1"}, "6");
  SyncClient();

  DoRedisTestInt(__LINE__, {"HINCRBY", "h1", "f1", "4"}, 10);
  DoRedisTestBulkString(__LINE__, {"HGET", "h1", "f1"}, "10");
  SyncClient();

  DoRedisTestInt(__LINE__, {"HINCRBY", "h1", "f1", "-5"}, 5);
  DoRedisTestBulkString(__LINE__, {"HGET", "h1", "f1"}, "5");

  DoRedisTestInt(__LINE__, {"HINCRBY", "h1", "fne", "6"}, 6);
  DoRedisTestBulkString(__LINE__, {"HGET", "h1", "fne"}, "6");
  SyncClient();

  DoRedisTestInt(__LINE__, {"HINCRBY", "hstr", "fstr", "5"}, 5);
  DoRedisTestBulkString(__LINE__, {"HGET", "hstr", "fstr"}, "5");
  SyncClient();

  DoRedisTestNull(__LINE__, {"GET", "hne1"});
  DoRedisTestInt(__LINE__, {"HINCRBY", "hne1", "fne", "6"}, 6);
  DoRedisTestBulkString(__LINE__, {"HGET", "hne1", "fne"}, "6");

  DoRedisTestInt(__LINE__, {"HINCRBY", "hne1", "fne", "-16"}, -10);
  DoRedisTestBulkString(__LINE__, {"HGET", "hne1", "fne"}, "-10");

  SyncClient();
  LOG(INFO) << "Done with the test";
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestKeysPipeline) {
  DoRedisTestOk(__LINE__, {"SET", "xa", "5"});
  DoRedisTestArray(__LINE__, {"KEYS", "*"}, {"xa"});
  DoRedisTestNull(__LINE__, {"GET", "xb"});
  DoRedisTestBulkString(__LINE__, {"GET", "xa"}, "5");
  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestIncrCorner) {
  DoRedisTestOk(__LINE__, {"SET", "kstr", "str"});
  SyncClient();

  DoRedisTestExpectError(__LINE__, {"INCR", "kstr"}, "ERR");
  DoRedisTestBulkString(__LINE__, {"GET", "kstr"}, "str");
  DoRedisTestExpectError(__LINE__, {"INCRBY", "kstr", "5"}, "ERR");
  DoRedisTestBulkString(__LINE__, {"GET", "kstr"}, "str");
  SyncClient();

  DoRedisTestInt(__LINE__, {"HSET", "h1", "f1", "5"}, 1);
  DoRedisTestInt(__LINE__, {"HSET", "h1", "fstr", "str"}, 1);
  SyncClient();

  // over 32 bit
  DoRedisTestOk(__LINE__, {"SET", "novar", "17179869184"});
  SyncClient();
  DoRedisTestInt(__LINE__, {"INCR", "novar"}, 17179869185);
  SyncClient();
  DoRedisTestInt(__LINE__, {"INCRBY", "novar", "17179869183"}, 34359738368);
  SyncClient();

  // over 32 bit
  DoRedisTestOk(__LINE__, {"SET", "novar64", "9223372036854775807"}); // 2 ** 63 - 1
  SyncClient();
  DoRedisTestExpectError(__LINE__, {"INCR", "novar64"}, "Increment would overflow");
  SyncClient();

  // INCRBY on a Hash type should fail.
  DoRedisTestExpectError(__LINE__, {"INCRBY", "h1", "5"}, "WRONGTYPE");
  SyncClient();
  // HINCRBY should fail on a normal key.
  DoRedisTestExpectError(__LINE__, {"HINCRBY", "kstr", "fstr", "5"}, "WRONGTYPE");
  SyncClient();
  // HINCRBY too many arguments.
  DoRedisTestExpectError(__LINE__, {"HINCRBY", "h1", "f1", "5", "extra_arg"});
  SyncClient();

  DoRedisTestExpectError(__LINE__, {"HINCRBY", "h1", "fstr", "5"}, "ERR");
  DoRedisTestBulkString(__LINE__, {"HGET", "h1", "fstr"}, "str");
  SyncClient();

  VerifyCallbacks();
}

// This test also uses the open source client
TEST_F(TestRedisService, TestTtlSetEx) {

  DoRedisTestOk(__LINE__, {"SET", "k1", "v1"});
  DoRedisTestOk(__LINE__, {"SET", "k2", "v2", "EX", "1"});
  DoRedisTestOk(__LINE__, {"SET", "k3", "v3", "EX", NonTsanVsTsan("20", "100")});
  DoRedisTestOk(__LINE__, {"SET", "k4", "v4", "EX", std::to_string(kRedisMaxTtlSeconds)});
  DoRedisTestOk(__LINE__, {"SET", "k5", "v5", "EX", std::to_string(kRedisMinTtlSetExSeconds)});

  // Invalid ttl.
  DoRedisTestExpectError(__LINE__, {"SET", "k6", "v6", "EX",
      std::to_string(kRedisMaxTtlSeconds + 1)});
  DoRedisTestExpectError(__LINE__, {"SET", "k7", "v7", "EX",
      std::to_string(kRedisMinTtlSetExSeconds - 1)});

  // Commands are pipelined and only sent when client.commit() is called.
  // sync_commit() waits until all responses are received.
  SyncClient();
  std::this_thread::sleep_for(2s);

  DoRedisTestBulkString(__LINE__, {"GET", "k1"}, "v1");
  DoRedisTestNull(__LINE__, {"GET", "k2"});
  DoRedisTestBulkString(__LINE__, {"GET", "k3"}, "v3");
  DoRedisTestBulkString(__LINE__, {"GET", "k4"}, "v4");
  DoRedisTestNull(__LINE__, {"GET", "k5"});

  SyncClient();
  DoRedisTestOk(__LINE__, {"SET", "k10", "v10", "EX", "5", "NX"});
  SyncClient();
  DoRedisTestBulkString(__LINE__, {"GET", "k10"}, "v10");
  SyncClient();

  std::this_thread::sleep_for(10s);
  DoRedisTestOk(__LINE__, {"SET", "k10", "v10", "EX", "5", "NX"});

  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestDummyLocal) {
  expected_no_sessions_ = true;
  DoRedisTestBulkString(__LINE__, {"INFO"}, kInfoResponse);
  DoRedisTestBulkString(__LINE__, {"INFO", "Replication"}, kInfoResponse);
  DoRedisTestBulkString(__LINE__, {"INFO", "foo", "bar", "whatever", "whatever"}, kInfoResponse);
  DoRedisTest(__LINE__, {"INFO"}, RedisReplyType::kString, [] (const RedisReply& reply) {
      ASSERT_NE(reply.as_string().find("redis_version"), string::npos);
    }
  );

  DoRedisTestOk(__LINE__, {"COMMAND"});
  DoRedisTestExpectError(__LINE__, {"EVAL"});

  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestTimeSeriesTtl) {
  FLAGS_emulate_redis_responses = true;
  DoRedisTestOk(__LINE__, {"TSADD", "key", "10", "v", "EXPIRE_IN", "5"});
  SyncClient();

  std::this_thread::sleep_for(std::chrono::seconds(10));
  DoRedisTestNull(__LINE__, {"TSGET", "key", "10"});
  DoRedisTestExpectError(__LINE__, {"ZADD", "key", "2", "val"});
  DoRedisTestOk(__LINE__, {"TSADD", "key", "20", "v"});

  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestTimeSeries) {
  // The default value is true, but we explicitly set this here for clarity.
  FLAGS_emulate_redis_responses = true;

  // Need an int for timeseries as a score.
  DoRedisTestExpectError(__LINE__, {"TSADD", "ts_key", "42.0", "42"});
  DoRedisTestExpectError(__LINE__, {"TSADD", "ts_key", "12.0", "42"});
  DoRedisTestExpectError(__LINE__, {"TSADD", "ts_key", "subkey1", "42"});
  DoRedisTestExpectError(__LINE__, {"TSADD", "ts_key", "subkey2", "12"});
  DoRedisTestExpectError(__LINE__, {"TSGET", "ts_key", "subkey1"});
  DoRedisTestExpectError(__LINE__, {"TSGET", "ts_key", "subkey2"});
  DoRedisTestExpectError(__LINE__, {"TSADD", "ts_key", "1", "v1", "2", "v2", "3.0", "v3"});
  DoRedisTestExpectError(__LINE__, {"TSADD", "ts_key", "1", "v1", "2", "v2", "abc", "v3"});
  DoRedisTestExpectError(__LINE__, {"TSADD", "ts_key", "1", "v1", "2", "v2", "123abc", "v3"});
  DoRedisTestExpectError(__LINE__, {"TSADD", "ts_key", "1", "v1", "2", "v2", " 123", "v3"});
  DoRedisTestExpectError(__LINE__, {"TSADD", "ts_key", "1", "v1", "2", "v2", "0xff", "v3"});

  // Incorrect number of arguments.
  DoRedisTestExpectError(__LINE__, {"TSADD", "ts_key", "subkey1"});
  DoRedisTestExpectError(__LINE__, {"TSADD", "ts_key", "subkey2"});
  DoRedisTestExpectError(__LINE__, {"TSGET", "ts_key"});
  DoRedisTestExpectError(__LINE__, {"TSGET", "ts_key"});
  DoRedisTestExpectError(__LINE__, {"TSADD", "ts_key", "1", "v1", "2", "v2", "3"});

  // Valid statements.
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key", "-10", "value1"});
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key", "-20", "value2"});
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key", "-30", "value3"});
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key", "10", "value4"});
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key", "20", "value5"});
  // For duplicate keys, the last one is picked up.
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key", "30", "value100", "30", "value6"});
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key", int64Max_, "valuemax"});
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key", int64Min_, "valuemin"});
  SyncClient();
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key", "30", "value7"});
  DoRedisTestOk(__LINE__, {"TSADD", "ts_multi", "10", "v1", "20", "v2", "30", "v3", "40", "v4"});
  DoRedisTestOk(__LINE__, {"TSADD", "ts_multi", "10", "v5", "50", "v6", "30", "v7", "60", "v8"});
  SyncClient();
  DoRedisTestOk(__LINE__, {"TSADD", "ts_multi", "10", "v9", "70", "v10", "30", "v11", "80", "v12"});
  SyncClient();

  // Ensure we retrieve appropriate results.
  DoRedisTestBulkString(__LINE__, {"TSGET", "ts_key", "-10"}, "value1");
  DoRedisTestBulkString(__LINE__, {"TSGET", "ts_key", "-20"}, "value2");
  DoRedisTestBulkString(__LINE__, {"TSGET", "ts_key", "-30"}, "value3");
  DoRedisTestBulkString(__LINE__, {"TSGET", "ts_key", "10"}, "value4");
  DoRedisTestBulkString(__LINE__, {"TSGET", "ts_key", "20"}, "value5");
  DoRedisTestBulkString(__LINE__, {"TSGET", "ts_key", "30"}, "value7");
  DoRedisTestBulkString(__LINE__, {"TSGET", "ts_key", int64Max_}, "valuemax");
  DoRedisTestBulkString(__LINE__, {"TSGET", "ts_key", int64Min_}, "valuemin");
  DoRedisTestBulkString(__LINE__, {"TSGET", "ts_multi", "10"}, "v9");
  DoRedisTestBulkString(__LINE__, {"TSGET", "ts_multi", "20"}, "v2");
  DoRedisTestBulkString(__LINE__, {"TSGET", "ts_multi", "30"}, "v11");
  DoRedisTestBulkString(__LINE__, {"TSGET", "ts_multi", "40"}, "v4");
  DoRedisTestBulkString(__LINE__, {"TSGET", "ts_multi", "50"}, "v6");
  DoRedisTestBulkString(__LINE__, {"TSGET", "ts_multi", "60"}, "v8");
  DoRedisTestBulkString(__LINE__, {"TSGET", "ts_multi", "70"}, "v10");
  DoRedisTestBulkString(__LINE__, {"TSGET", "ts_multi", "80"}, "v12");

  // Keys that are not present.
  DoRedisTestNull(__LINE__, {"TSGET", "ts_key", "40"});
  DoRedisTestNull(__LINE__, {"TSGET", "abc", "30"});

  // HGET/SISMEMBER/GET should not work with this.
  DoRedisTestExpectError(__LINE__, {"HGET", "ts_key", "30"});
  DoRedisTestExpectError(__LINE__, {"SISMEMBER", "ts_key", "30"});
  DoRedisTestExpectError(__LINE__, {"HEXISTS", "ts_key", "30"});
  DoRedisTestExpectError(__LINE__, {"GET", "ts_key"});

  // TSGET should not work with HSET.
  DoRedisTestInt(__LINE__, {"HSET", "map_key", "30", "v1"}, 1);
  DoRedisTestExpectError(__LINE__, {"TSGET", "map_key", "30"});

  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestSortedSets) {
  // The default value is true, but we explicitly set this here for clarity.
  FLAGS_emulate_redis_responses = true;

  // Need an double for sorted sets as a score.
  DoRedisTestExpectError(__LINE__, {"ZADD", "z_key", "subkey1", "42"});
  DoRedisTestExpectError(__LINE__, {"ZADD", "z_key", "subkey2", "12"});
  DoRedisTestExpectError(__LINE__, {"ZADD", "z_key", "1", "v1", "2", "v2", "abc", "v3"});
  DoRedisTestExpectError(__LINE__, {"ZADD", "z_key", "1", "v1", "2", "v2", "123abc", "v3"});
  DoRedisTestExpectError(__LINE__, {"ZADD", "z_key", "1", "v1", "2", "v2", " 123", "v3"});
  DoRedisTestExpectError(__LINE__, {"ZRANGEBYSCORE", "z_key", "1", " 2"});
  DoRedisTestExpectError(__LINE__, {"ZRANGEBYSCORE", "z_key", "1", "abc"});
  DoRedisTestExpectError(__LINE__, {"ZRANGEBYSCORE", "z_key", "abc", "2"});

  // Incorrect number of arguments.
  DoRedisTestExpectError(__LINE__, {"ZADD", "z_key", "subkey1"});
  DoRedisTestExpectError(__LINE__, {"ZADD", "z_key", "1", "v1", "2", "v2", "3"});
  DoRedisTestExpectError(__LINE__, {"ZRANGEBYSCORE", "z_key", "1"});
  DoRedisTestExpectError(__LINE__, {"ZRANGEBYSCORE", "z_key", "1", "2", "3"});
  DoRedisTestExpectError(__LINE__, {"ZRANGEBYSCORE", "z_key", "1", "2", "WITHSCORES", "abc"});
  DoRedisTestExpectError(__LINE__, {"ZREM", "z_key"});

  // Valid statements
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "-30.0", "v1"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "-20.0", "v2"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "-10.0", "v3"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "10.0", "v4"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "20.0", "v5"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "30.0", "v6"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key",
      strings::Substitute("$0", std::numeric_limits<double>::max()), "vmax"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key",
      strings::Substitute("$0",  -std::numeric_limits<double>::max()), "vmin"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "40.0", "v6"}, 0);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "0x1e", "v6"}, 0);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "-20", "v1"}, 0);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "-30", "v1"}, 0);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "30.000001", "v7"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "30.000001", "v8"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZCARD", "z_key"}, 10);
  DoRedisTestOk(__LINE__, {"SET", "s_key", "s_val"});
  SyncClient();
  DoRedisTestExpectError(__LINE__, {"ZCARD", "s_key"});
  DoRedisTestExpectErrorMsg(__LINE__, {"ZCARD", "s_key"},
      "WRONGTYPE Operation against a key holding the wrong kind of value");

  DoRedisTestInt(__LINE__, {"ZADD", "z_multi", "-10.0", "v3", "-20.0", "v2", "-30.0", "v1",
      "10.0", "v4", "20.0", "v5", "30.0", "v6"}, 6);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_multi", "40.0", "v6", "0x1e", "v6", "-20", "v1", "-30", "v1",
      "30.000001", "v7", "30.000001", "v8"}, 2);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_multi",
      strings::Substitute("$0", std::numeric_limits<double>::max()), "vmax",
      strings::Substitute("$0", -std::numeric_limits<double>::max()), "vmin"}, 2);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZCARD", "z_multi"}, 10);

  // Ensure we retrieve appropriate results.
  LOG(INFO) << "Starting ZRANGE queries";
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "+inf", "-inf"}, {});
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "-inf", "+inf"},
      {"vmin", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "vmax"});
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "(-inf", "(+inf"},
      {"vmin", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "vmax"});
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "20.0", "30.0"}, {"v5", "v6"});
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "20.0", "30.000001"},
      {"v5", "v6", "v7", "v8"});
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "20.0", "(30.000001"}, {"v5", "v6"});
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "(20.0", "30.000001"}, {"v6", "v7", "v8"});
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "-20.0", "-10.0"}, {"v2", "v3"});
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "(-20.0", "(-10.0"}, {});
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "+inf", "-inf"}, {});

  DoRedisTestScoreValueArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "20.0", "30.0", "WITHSCORES"},
      {20.0, 30.0}, {"v5", "v6"});
  DoRedisTestScoreValueArray(__LINE__,
      {"ZRANGEBYSCORE", "z_key", "20.0", "30.000001", "withscores"},
      {20.0, 30.0, 30.000001, 30.000001}, {"v5", "v6", "v7", "v8"});

  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_multi", "-inf", "+inf"},
      {"vmin", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "vmax"});
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_multi", "(-inf", "(+inf"},
      {"vmin", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "vmax"});
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_multi", "20.0", "30.0"}, {"v5", "v6"});
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_multi", "20.0", "30.000001"},
      {"v5", "v6", "v7", "v8"});
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_multi", "20.0", "(30.000001"}, {"v5", "v6"});
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_multi", "(20.0", "30.000001"},
      {"v6", "v7", "v8"});
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_multi", "-20.0", "-10.0"}, {"v2", "v3"});
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_multi", "(-20.0", "(-10.0"}, {});

  DoRedisTestScoreValueArray(__LINE__, {"ZRANGEBYSCORE", "z_multi", "20.0", "30.0", "WITHSCORES"},
      {20.0, 30.0}, {"v5", "v6"});
  DoRedisTestScoreValueArray(__LINE__,
      {"ZRANGEBYSCORE", "z_multi", "20.0", "30.000001", "withscores"},
      {20.0, 30.0, 30.000001, 30.000001}, {"v5", "v6", "v7", "v8"});

  DoRedisTestInt(__LINE__, {"ZREM", "z_key", "v6"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZREM", "z_key", "v6"}, 0);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZREM", "z_key", "v7"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZREM", "z_key", "v9"}, 0);
  SyncClient();

  DoRedisTestInt(__LINE__, {"ZREM", "z_multi", "v6", "v7", "v9"}, 2);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZREM", "z_multi", "v6", "v7", "v9"}, 0);
  SyncClient();

  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "-inf", "+inf"},
      {"vmin", "v1", "v2", "v3", "v4", "v5", "v8", "vmax"});
  DoRedisTestInt(__LINE__, {"ZCARD", "z_key"}, 8);

  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_multi", "-inf", "+inf"},
      {"vmin", "v1", "v2", "v3", "v4", "v5", "v8", "vmax"});
  DoRedisTestInt(__LINE__, {"ZCARD", "z_multi"}, 8);

  // Test NX/CH option.
  LOG(INFO) << "Starting ZADD with options";
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "NX", "0", "v8"}, 0);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "NX", "CH", "0", "v8"}, 0);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "NX", "0", "v9"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "NX", "40", "v9"}, 0);
  SyncClient();

  // Make sure that only v9 exists at 0 and not at 40.
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "0.0", "0.0"}, {"v9"});
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "40.0", "40.0"}, {});
  DoRedisTestInt(__LINE__, {"ZCARD", "z_key"}, 9);

  // Test XX/CH option.
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "XX", "CH", "0", "v8"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "XX", "30.000001", "v8"}, 0);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "XX", "0", "v10"}, 0);
  SyncClient();

  // Make sure that only v9 exists at 0 and v8 exists at 30.000001.
  DoRedisTestScoreValueArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "0.0", "0.0", "WITHSCORES"},
      {0.0}, {"v9"});
  DoRedisTestScoreValueArray(__LINE__,
      {"ZRANGEBYSCORE", "z_key", "30.000001", "30.000001", "WITHSCORES"},
      {30.000001}, {"v8"});
  DoRedisTestInt(__LINE__, {"ZCARD", "z_key"}, 9);
  DoRedisTestInt(__LINE__, {"ZCARD", "does_not_exist"}, 0);

  // Test NX/XX/CH option for multi.
  DoRedisTestInt(__LINE__, {"ZADD", "z_multi", "NX", "0", "v8", "40", "v9"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_multi", "CH", "0", "v8", "0", "v9"}, 2);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_multi", "XX", "CH", "30.000001", "v8", "0", "v10"}, 1);
  SyncClient();

  // Make sure that only v9 exists and 0 and v8 exists at 30.000001.
  DoRedisTestScoreValueArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "0.0", "0.0", "WITHSCORES"},
      {0.0}, {"v9"});
  DoRedisTestScoreValueArray(__LINE__,
      {"ZRANGEBYSCORE", "z_key", "30.000001", "30.000001", "WITHSCORES"},
      {30.000001}, {"v8"});
  DoRedisTestInt(__LINE__, {"ZCARD", "z_multi"}, 9);

  // Test incr option.
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "INCR", "10", "v8"}, 0);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "INCR", "XX", "CH", "10", "v8"}, 1);
  SyncClient();
  // This shouldn't do anything, since NX option is specified.
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "INCR", "NX", "10", "v8"}, 0);
  SyncClient();

  // Make sure v8 has been incremented by 20.
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "30.000001", "30.000001"}, {});
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "50.000001", "50.000001"}, {"v8"});
  DoRedisTestInt(__LINE__, {"ZCARD", "z_key"}, 9);

  // HGET/SISMEMBER/GET/TS should not work with this.
  DoRedisTestExpectError(__LINE__, {"SISMEMBER", "z_key", "30"});
  DoRedisTestExpectError(__LINE__, {"HEXISTS", "z_key", "30"});
  DoRedisTestExpectError(__LINE__, {"GET", "z_key"});
  DoRedisTestExpectError(__LINE__, {"TSRANGE", "z_key", "1", "a"});

  // ZADD should not work with HSET.
  DoRedisTestInt(__LINE__, {"HSET", "map_key", "30", "v1"}, 1);
  DoRedisTestExpectError(__LINE__, {"ZADD", "map_key", "40", "v2"});

  // Cannot have both NX and XX options.
  DoRedisTestExpectError(__LINE__, {"ZADD", "z_key", "CH", "NX", "XX", "0", "v1"});

  DoRedisTestExpectError(__LINE__, {"ZADD", "z_key", "CH", "NX", "INCR"});
  DoRedisTestExpectError(__LINE__, {"ZADD", "z_key", "XX"});
  DoRedisTestExpectError(__LINE__, {"ZADD", "z_key", "CH", "NX", "0", "v1", "1"});
  // Cannot have incr with multiple score value pairs.
  DoRedisTestExpectError(__LINE__, {"ZADD", "z_key", "INCR", "0", "v1", "1", "v2"});

  // Test ZREM on non-existent key and then add the same key.
  DoRedisTestInt(__LINE__, {"ZREM", "my_z_set", "v1"}, 0);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZCARD", "my_z_set"}, 0);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "my_z_set", "1", "v1"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZCARD", "my_z_set"}, 1);
  SyncClient();
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "my_z_set", "1", "1"}, {"v1"});
}

TEST_F(TestRedisService, ZRangeByScoreInvalidOptions) {
  expected_no_sessions_ = true;

  // Not enough args to ZRANGEBYSCORE should throw an error.
  DoRedisTestExpectError(__LINE__, {"ZRANGEBYSCORE", "z_key", "-inf", "+inf", "LIMIT"});
  DoRedisTestExpectError(__LINE__, {"ZRANGEBYSCORE", "z_key", "-inf", "+inf", "LIMIT", "2"});
  DoRedisTestExpectError(__LINE__, {"ZRANGEBYSCORE", "z_key", "-inf", "+inf", "LIMIT", "a", "1"});
  DoRedisTestExpectError(__LINE__, {
      "ZRANGEBYSCORE", "z_key", "-inf", "+inf", "LIMIT", "1", "2", "3"});
  DoRedisTestExpectError(__LINE__, {
      "ZRANGEBYSCORE", "z_key", "-inf", "+inf", "LIMIT", "WITHSCORES", "2", "3"});
  DoRedisTestExpectError(__LINE__, {
      "ZRANGEBYSCORE", "z_key", "-inf", "+inf", "WITHSCORES", "2", "3"});
  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestZRevRange) {
  // The default value is true, but we explicitly set this here for clarity.
  FLAGS_emulate_redis_responses = true;
  DoRedisTestInt(__LINE__, {"ZADD", "z_multi", "0", "v0", "0", "v1", "0", "v2",
      "1", "v3", "1", "v4", "1", "v5"}, 6);
  SyncClient();

  DoRedisTestArray(__LINE__, {"ZREVRANGE", "z_multi", "0", "5"},
                   {"v5", "v4", "v3", "v2", "v1", "v0"});
  DoRedisTestScoreValueArray(__LINE__, {"ZREVRANGE", "z_multi", "0", "-1", "WITHSCORES"},
                             {1, 1, 1, 0, 0, 0},
                             {"v5", "v4", "v3", "v2", "v1", "v0"});
  DoRedisTestArray(__LINE__, {"ZREVRANGE", "z_multi", "0", "1"}, {"v5", "v4"});
  DoRedisTestArray(__LINE__, {"ZREVRANGE", "z_multi", "2", "3"}, {"v3", "v2"});
  DoRedisTestArray(__LINE__, {"ZREVRANGE", "z_multi", "6", "7"}, {});
  DoRedisTestArray(__LINE__, {"ZREVRANGE", "z_multi", "0", "-1"},
                   {"v5", "v4", "v3", "v2", "v1", "v0"});
  DoRedisTestArray(__LINE__, {"ZREVRANGE", "z_multi", "-2", "-1"}, {"v1", "v0"});
  DoRedisTestArray(__LINE__, {"ZREVRANGE", "z_multi", "-3", "-2"}, {"v2", "v1"});
  DoRedisTestArray(__LINE__, {"ZREVRANGE", "z_multi", "-3", "5"}, {"v2", "v1", "v0"});

  // Test empty key.
  DoRedisTestArray(__LINE__, {"ZREVRANGE", "z_key", "0", "1"}, {});

  DoRedisTestExpectError(__LINE__, {"ZREVRANGE", "z_multi", "0"});
  DoRedisTestExpectError(__LINE__, {"ZREVRANGE", "z_multi", "1", "2", "3"});
  DoRedisTestExpectError(__LINE__, {"ZREVRANGE", "z_multi", "1", "2", "WITHSCORES", "1"});
  DoRedisTestExpectError(__LINE__, {"ZREVRANGE", "z_multi", "1.0", "2.0"});
  DoRedisTestExpectError(__LINE__, {"ZREVRANGE", "1", "2"});
  DoRedisTestExpectError(__LINE__, {"ZREVRANGE", "z_multi", "0", "(2"});
  DoRedisTestExpectError(__LINE__, {"ZREVRANGE", "z_multi", "(0", "2"});
  DoRedisTestExpectError(__LINE__, {"ZREVRANGE", "z_multi", "(0", "(2"});

  // Test key with wrong type.
  DoRedisTestOk(__LINE__, {"SET", "s_key", "s_val"});
  DoRedisTestExpectError(__LINE__, {"ZREVRANGE", "s_key", "1", "2"});

  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestZRange) {
  // The default value is true, but we explicitly set this here for clarity.
  FLAGS_emulate_redis_responses = true;
  DoRedisTestInt(__LINE__, {"ZADD", "z_multi", "0", "v0", "0", "v1", "0", "v2",
      "1", "v3", "1", "v4", "1", "v5"}, 6);
  SyncClient();

  DoRedisTestArray(__LINE__, {"ZRANGE", "z_multi", "0", "5"}, {"v0", "v1", "v2", "v3", "v4", "v5"});
  DoRedisTestScoreValueArray(__LINE__, {"ZRANGE", "z_multi", "0", "-1", "WITHSCORES"},
                             {0, 0, 0, 1, 1, 1},
                             {"v0", "v1", "v2", "v3", "v4", "v5"});
  DoRedisTestArray(__LINE__, {"ZRANGE", "z_multi", "0", "1"}, {"v0", "v1"});
  DoRedisTestArray(__LINE__, {"ZRANGE", "z_multi", "2", "3"}, {"v2", "v3"});
  DoRedisTestArray(__LINE__, {"ZRANGE", "z_multi", "6", "7"}, {});
  DoRedisTestArray(__LINE__, {"ZRANGE", "z_multi", "0", "-1"},
                   {"v0", "v1", "v2", "v3", "v4", "v5"});
  DoRedisTestArray(__LINE__, {"ZRANGE", "z_multi", "-2", "-1"}, {"v4", "v5"});
  DoRedisTestArray(__LINE__, {"ZRANGE", "z_multi", "-3", "-2"}, {"v3", "v4"});
  DoRedisTestArray(__LINE__, {"ZRANGE", "z_multi", "-3", "5"}, {"v3", "v4", "v5"});
  DoRedisTestArray(__LINE__, {"ZRANGE", "z_multi", "0", "100"},
                   {"v0", "v1", "v2", "v3", "v4", "v5"});
  DoRedisTestArray(__LINE__, {"ZRANGE", "z_multi", "-100", "100"},
                   {"v0", "v1", "v2", "v3", "v4", "v5"});
  DoRedisTestArray(__LINE__, {"ZRANGE", "z_multi", "10", "100"}, {});
  DoRedisTestArray(__LINE__, {"ZRANGE", "z_multi", "-100", "-10"}, {});

  // Test empty key.
  DoRedisTestArray(__LINE__, {"ZRANGE", "z_key", "0", "1"}, {});

  DoRedisTestExpectError(__LINE__, {"ZRANGE", "z_multi", "0"});
  DoRedisTestExpectError(__LINE__, {"ZRANGE", "z_multi", "1", "2", "3"});
  DoRedisTestExpectError(__LINE__, {"ZRANGE", "z_multi", "1", "2", "WITHSCORES", "1"});
  DoRedisTestExpectError(__LINE__, {"ZRANGE", "z_multi", "1.0", "2.0"});
  DoRedisTestExpectError(__LINE__, {"ZRANGE", "1", "2"});

  // Test key with wrong type.
  DoRedisTestOk(__LINE__, {"SET", "s_key", "s_val"});
  DoRedisTestExpectError(__LINE__, {"ZRANGE", "s_key", "1", "2"});

  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestZScore) {
  // The default value is true, but we explicitly set this here for clarity.
  FLAGS_emulate_redis_responses = true;
  DoRedisTestInt(__LINE__, {"ZADD", "z_multi", "0", "v0", "0", "v0_copy", "1", "v1",
      "2", "v2", "3", "v3", "4.5", "v4"}, 6);
  SyncClient();

  DoRedisTestDouble(__LINE__, {"ZSCORE", "z_multi", "v0"}, 0.0);
  DoRedisTestDouble(__LINE__, {"ZSCORE", "z_multi", "v0_copy"}, 0.0);
  DoRedisTestDouble(__LINE__, {"ZSCORE", "z_multi", "v1"}, 1.0);
  DoRedisTestDouble(__LINE__, {"ZSCORE", "z_multi", "v2"}, 2.0);
  DoRedisTestDouble(__LINE__, {"ZSCORE", "z_multi", "v3"}, 3.0);
  DoRedisTestDouble(__LINE__, {"ZSCORE", "z_multi", "v4"}, 4.5);

  DoRedisTestNull(__LINE__, {"ZSCORE", "z_no_exist", "v4"});

  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestTimeSeriesTTL) {
  int64_t ttl_sec = 10;
  TestTSTtl("EXPIRE_IN", ttl_sec, ttl_sec, "test_expire_in");
  int64_t curr_time_sec = GetCurrentTimeMicros() / MonoTime::kMicrosecondsPerSecond;
  TestTSTtl("EXPIRE_AT", ttl_sec, curr_time_sec + ttl_sec, "test_expire_at");

  DoRedisTestExpectError(__LINE__, {"TSADD", "test_expire_in", "10", "v1", "EXPIRE_IN",
      std::to_string(kRedisMinTtlSetExSeconds - 1)});
  DoRedisTestExpectError(__LINE__, {"TSADD", "test_expire_in", "10", "v1", "EXPIRE_IN",
      std::to_string(kRedisMaxTtlSeconds + 1)});

  DoRedisTestExpectError(__LINE__, {"TSADD", "test_expire_at", "10", "v1", "EXPIRE_AT",
      std::to_string(curr_time_sec - 10)});
  DoRedisTestExpectError(__LINE__, {"TSADD", "test_expire_at", "10", "v1", "expire_at",
      std::to_string(curr_time_sec - 10)});

  DoRedisTestExpectError(__LINE__, {"TSADD", "test_expire_at", "10", "v1", "EXPIRE_AT",
      std::to_string(curr_time_sec + kRedisMinTtlSetExSeconds - 1)});
  DoRedisTestExpectError(__LINE__, {"TSADD", "test_expire_at", "10", "v1", "expire_at",
      std::to_string(curr_time_sec + kRedisMinTtlSetExSeconds - 1)});
  DoRedisTestExpectError(__LINE__, {"TSADD", "test_expire_at", "10", "v1", "exPiRe_aT",
                                    std::to_string(curr_time_sec + kRedisMinTtlSetExSeconds - 1)});

  DoRedisTestExpectError(__LINE__, {"TSADD", "test_expire_at", "10", "v1", "EXPIRE_IN",
      std::to_string(curr_time_sec + kRedisMaxTtlSeconds + 1)});
  DoRedisTestExpectError(__LINE__, {"TSADD", "test_expire_at", "10", "v1", "expire_in",
      std::to_string(curr_time_sec + kRedisMaxTtlSeconds + 1)});
  DoRedisTestExpectError(__LINE__, {"TSADD", "test_expire_at", "10", "v1", "eXpIrE_In",
                                    std::to_string(curr_time_sec + kRedisMaxTtlSeconds + 1)});
}

TEST_F(TestRedisService, TestTsCard) {
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key",
      "-50", "v1",
      "-40", "v2",
      "-30", "v3",
      "-20", "v4",
      "-10", "v5",
      "10", "v6",
      "20", "v7",
      "30", "v8",
      "40", "v9",
      "50", "v10",
  });

  DoRedisTestOk(__LINE__, {"TSADD", "ts_key1",
      "10", "v6",
      "20", "v7",
      "30", "v8",
      "40", "v9",
      "50", "v10",
  });

  DoRedisTestOk(__LINE__, {"TSADD", "ts_key2", "10", "v6"});
  SyncClient();
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key2", "11", "v7", "EXPIRE_IN", "10"});
  SyncClient();

  DoRedisTestInt(__LINE__, {"TSCARD", "ts_key"}, 10);
  SyncClient();
  DoRedisTestInt(__LINE__, {"TSCARD", "ts_key1"}, 5);
  SyncClient();
  DoRedisTestInt(__LINE__, {"TSCARD", "ts_key2"}, 2);
  SyncClient();
  DoRedisTestInt(__LINE__, {"TSCARD", "invalid_key"}, 0);
  SyncClient();

  // After TTL expiry.
  std::this_thread::sleep_for(std::chrono::seconds(11));
  DoRedisTestInt(__LINE__, {"TSCARD", "ts_key2"}, 1);

  // Test errors.
  DoRedisTestInt(__LINE__, {"ZADD", "z_multi", "0", "v0", "0", "v1", "0", "v2",
      "1", "v3", "1", "v4", "1", "v5"}, 6);
  DoRedisTestExpectError(__LINE__, {"TSCARD", "z_multi"}); // incorrect type.

  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestTsLastN) {
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key",
      "-50", "v1",
      "-40", "v2",
      "-30", "v3",
      "-20", "v4",
      "-10", "v5",
      "10", "v6",
      "20", "v7",
      "30", "v8",
      "40", "v9",
      "50", "v10",
  });

  SyncClient();
  DoRedisTestArray(__LINE__, {"TSLASTN", "ts_key", "5"},
                   {"10", "v6", "20", "v7", "30", "v8", "40", "v9", "50", "v10"});
  DoRedisTestArray(__LINE__, {"TSLASTN", "ts_key", "4"},
                   {"20", "v7", "30", "v8", "40", "v9", "50", "v10"});
  DoRedisTestArray(__LINE__, {"TSLASTN", "ts_key", "3"},
                   {"30", "v8", "40", "v9", "50", "v10"});
  DoRedisTestArray(__LINE__, {"TSLASTN", "ts_key", "2"},
                   {"40", "v9", "50", "v10"});
  DoRedisTestArray(__LINE__, {"TSLASTN", "ts_key", "10"},
                   {"-50", "v1", "-40", "v2", "-30", "v3", "-20", "v4", "-10", "v5", "10", "v6",
                       "20", "v7", "30", "v8", "40", "v9", "50", "v10"});
  DoRedisTestArray(__LINE__, {"TSLASTN", "ts_key", "20"},
                   {"-50", "v1", "-40", "v2", "-30", "v3", "-20", "v4", "-10", "v5", "10", "v6",
                       "20", "v7", "30", "v8", "40", "v9", "50", "v10"});
  DoRedisTestArray(__LINE__, {"TSLASTN", "ts_key",
                       std::to_string(std::numeric_limits<int32>::max())},
                   {"-50", "v1", "-40", "v2", "-30", "v3", "-20", "v4", "-10", "v5", "10", "v6",
                       "20", "v7", "30", "v8", "40", "v9", "50", "v10"});

  DoRedisTestExpectError(__LINE__, {"TSLASTN", "ts_key", "abc"});
  DoRedisTestExpectError(__LINE__, {"TSLASTN", "ts_key", "3.0"});
  DoRedisTestExpectError(__LINE__, {"TSLASTN", "ts_key", "999999999999"}); // out of bounds.
  DoRedisTestExpectError(__LINE__, {"TSLASTN", "ts_key", "-999999999999"}); // out of bounds.
  DoRedisTestExpectError(__LINE__, {"TSLASTN", "ts_key", "0"}); // out of bounds.
  DoRedisTestExpectError(__LINE__, {"TSLASTN", "ts_key", "-1"}); // out of bounds.
  DoRedisTestNull(__LINE__, {"TSLASTN", "randomkey", "10"}); // invalid key.
  DoRedisTestInt(__LINE__, {"ZADD", "z_multi", "0", "v0", "0", "v1", "0", "v2",
      "1", "v3", "1", "v4", "1", "v5"}, 6);
  DoRedisTestExpectError(__LINE__, {"TSLASTN", "z_multi", "10"}); // incorrect type.
  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestTsRangeByTime) {
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key",
      "-50", "v1",
      "-40", "v2",
      "-30", "v3",
      "-20", "v4",
      "-10", "v5",
      "10", "v6",
      "20", "v7",
      "30", "v8",
      "40", "v9",
      "50", "v10",
  });

  SyncClient();
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-35", "25"},
      {"-30", "v3", "-20", "v4", "-10", "v5", "10", "v6", "20", "v7"});

  // Overwrite and test.
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key",
      "-50", "v11",
      "-40", "v22",
      "-30", "v33",
      "-20", "v44",
      "-10", "v55",
      "10", "v66",
      "20", "v77",
      "30", "v88",
      "40", "v99",
      "50", "v110",
  });

  SyncClient();
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-55", "-10"},
      {"-50", "v11", "-40", "v22", "-30", "v33", "-20", "v44", "-10", "v55"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-20", "55"},
      {"-20", "v44", "-10", "v55", "10", "v66", "20", "v77", "30", "v88", "40", "v99",
          "50", "v110"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-55", "55"},
      {"-50", "v11", "-40", "v22", "-30", "v33", "-20", "v44", "-10", "v55",
          "10", "v66", "20", "v77", "30", "v88", "40", "v99", "50", "v110"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-15", "-5"}, {"-10", "v55"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "10", "10"}, {"10", "v66"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-10", "-10"}, {"-10", "v55"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-57", "-55"}, {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "55", "60"}, {});

  // Test with ttl.
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key",
      "-30", "v333",
      "-10", "v555",
      "20", "v777",
      "30", "v888",
      "50", "v1110",
      "EXPIRE_IN", "5",
  });
  SyncClient();
  std::this_thread::sleep_for(std::chrono::seconds(6));
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-55", "-10"},
      {"-50", "v11", "-40", "v22", "-20", "v44"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-20", "55"},
      {"-20", "v44", "10", "v66", "40", "v99"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-55", "60"},
      {"-50", "v11", "-40", "v22", "-20", "v44", "10", "v66", "40", "v99"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-15", "-5"}, {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "10", "10"}, {"10", "v66"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-25", "-15"}, {"-20", "v44"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-5", "-15"}, {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-45", "-55"}, {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "45", "55"}, {});

  // Test exclusive ranges.
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "(-20", "(40"}, {"10", "v66"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "(-20", "(-20"}, {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "(-20", "-10"}, {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-10", "(10"}, {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "(-50", "(-40"}, {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-55", "(11"},
      {"-50", "v11", "-40", "v22", "-20", "v44", "10", "v66"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "(-50", "10"},
      {"-40", "v22", "-20", "v44", "10", "v66"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "(-51", "10"},
      {"-50", "v11", "-40", "v22", "-20", "v44", "10", "v66"});

  // Test infinity.
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-10", "+inf"},
      {"10", "v66", "40", "v99"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-inf", "10"},
      {"-50", "v11", "-40", "v22", "-20", "v44", "10", "v66"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-10", "(+inf"},
      {"10", "v66", "40", "v99"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "(-inf", "10"},
      {"-50", "v11", "-40", "v22", "-20", "v44", "10", "v66"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-inf", "+inf"},
      {"-50", "v11", "-40", "v22", "-20", "v44", "10", "v66", "40", "v99"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "(-inf", "(+inf"},
      {"-50", "v11", "-40", "v22", "-20", "v44", "10", "v66", "40", "v99"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "+inf", "-inf"}, {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "+inf", "10"}, {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "+inf", "+inf"}, {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "10", "-inf"}, {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-inf", "-inf"}, {});
  SyncClient();

  // Test infinity with int64 min, max.
  DoRedisTestOk(__LINE__, {"TSADD", "ts_inf",
      int64Min_, "v1",
      "-10", "v2",
      "10", "v3",
      int64Max_, "v4",
  });
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_inf", "-inf", "+inf"},
      {int64Min_, "v1", "-10", "v2", "10", "v3", int64Max_, "v4"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_inf", "(-inf", "(+inf"},
      {int64Min_, "v1", "-10", "v2", "10", "v3", int64Max_, "v4"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_inf", "-inf", "-inf"},
      {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_inf", "+inf", "+inf"},
      {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_inf", "-10", "(+inf"},
      {"-10", "v2", "10", "v3", int64Max_, "v4"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_inf", "-10", "+inf"},
      {"-10", "v2", "10", "v3", int64Max_, "v4"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_inf", "(-inf", "10"},
      {int64Min_, "v1", "-10", "v2", "10", "v3"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_inf", "-inf", "10"},
      {int64Min_, "v1", "-10", "v2", "10", "v3"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_inf", int64Min_, int64Max_},
      {int64Min_, "v1", "-10", "v2", "10", "v3", int64Max_, "v4"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_inf", int64MaxExclusive_, int64Max_},
      {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_inf", int64MaxExclusive_, int64MaxExclusive_},
      {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_inf", int64Max_, int64MaxExclusive_},
      {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_inf", int64MinExclusive_, int64MinExclusive_},
      {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_inf", int64MinExclusive_, int64Min_},
      {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_inf", int64Min_, int64MinExclusive_},
      {});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_inf", int64Min_, int64Min_},
      {int64Min_, "v1"});
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_inf", int64Max_, int64Max_},
      {int64Max_, "v4"});

  // Test invalid requests.
  DoRedisTestExpectError(__LINE__, {"TSRANGEBYTIME", "ts_key", "10", "20", "30"});
  DoRedisTestExpectError(__LINE__, {"TSRANGEBYTIME", "ts_key", "10", "abc"});
  DoRedisTestExpectError(__LINE__, {"TSRANGEBYTIME", "ts_key", "10", "20.1"});
  DoRedisTestOk(__LINE__, {"HMSET", "map_key",
      "1", "v100",
      "2", "v200",
      "3", "v300",
      "4", "v400",
      "5", "v500",
  });
  DoRedisTestOk(__LINE__, {"HMSET", "map_key",
      "6", "v600"
  });
  DoRedisTestOk(__LINE__, {"SET", "key", "value"});
  SyncClient();
  DoRedisTestExpectError(__LINE__, {"TSRANGEBYTIME", "map_key", "1", "5"});
  DoRedisTestExpectError(__LINE__, {"TSRANGEBYTIME", "key"});

  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestTsRevRangeByTime) {
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key",
      "-50", "v1",
      "-40", "v2",
      "-30", "v3",
      "-20", "v4",
      "-10", "v5",
      "10", "v6",
      "20", "v7",
      "30", "v8",
      "40", "v9",
      "50", "v10",
  });

  SyncClient();
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-35", "25"},
                   {"20", "v7", "10", "v6", "-10", "v5", "-20", "v4", "-30", "v3"});

  // Overwrite and test.
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key",
      "-50", "v11",
      "-40", "v22",
      "-30", "v33",
      "-20", "v44",
      "-10", "v55",
      "10", "v66",
      "20", "v77",
      "30", "v88",
      "40", "v99",
      "50", "v110",
  });

  SyncClient();
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-55", "-10"},
                   {"-10", "v55", "-20", "v44", "-30", "v33", "-40", "v22", "-50", "v11"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-20", "55"},
                   {"50", "v110", "40", "v99", "30", "v88", "20", "v77", "10", "v66", "-10",
                    "v55", "-20", "v44"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-55", "55"},
                   {"50", "v110", "40", "v99", "30", "v88", "20", "v77", "10", "v66", "-10",
                    "v55", "-20", "v44", "-30", "v33", "-40", "v22", "-50", "v11"});

  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-15", "-5"}, {"-10", "v55"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "10", "10"}, {"10", "v66"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-10", "-10"}, {"-10", "v55"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-57", "-55"}, {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "55", "60"}, {});

  // Test with limit.
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-55", "-10", "LIMIT", "1"},
                   {"-10", "v55"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-55", "-10", "LIMIT", "2"},
                   {"-10", "v55", "-20", "v44"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-20", "55", "LIMIT", "4"},
                   {"50", "v110", "40", "v99", "30", "v88", "20", "v77"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-55", "55", "LIMIT", "5"},
                   {"50", "v110", "40", "v99", "30", "v88", "20", "v77", "10", "v66"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-inf", "+inf", "LIMIT", "1"},
                   {"50", "v110"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-inf", "(50", "LIMIT", "1"},
                   {"40", "v99"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-inf", "(40", "LIMIT", "1"},
                   {"30", "v88"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-inf", "(30", "LIMIT", "1"},
                   {"20", "v77"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-inf", "(20", "LIMIT", "1"},
                   {"10", "v66"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-inf", "(10", "LIMIT", "1"},
                   {"-10", "v55"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-inf", "(-10", "LIMIT", "1"},
                   {"-20", "v44"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-inf", "(-20", "LIMIT", "1"},
                   {"-30", "v33"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-inf", "(-30", "LIMIT", "1"},
                   {"-40", "v22"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-inf", "(-40", "LIMIT", "1"},
                   {"-50", "v11"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-inf", "(-50", "LIMIT", "1"},
                   {});

  // Test with a limit larger than the total number of elements.
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-55", "-10", "LIMIT", "300"},
                   {"-10", "v55", "-20", "v44", "-30", "v33", "-40", "v22", "-50", "v11"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-20", "55", "LIMIT", "121"},
                   {"50", "v110", "40", "v99", "30", "v88", "20", "v77", "10", "v66", "-10",
                    "v55", "-20", "v44"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-55", "55", "LIMIT", "34"},
                   {"50", "v110", "40", "v99", "30", "v88", "20", "v77", "10", "v66", "-10",
                    "v55", "-20", "v44", "-30", "v33", "-40", "v22", "-50", "v11"});

  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-15", "-5"}, {"-10", "v55"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "10", "10"}, {"10", "v66"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-10", "-10"}, {"-10", "v55"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-57", "-55"}, {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "55", "60"}, {});

  // Test with ttl.
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key",
      "-30", "v333",
      "-10", "v555",
      "20", "v777",
      "30", "v888",
      "50", "v1110",
      "EXPIRE_IN", "5",
  });
  SyncClient();
  std::this_thread::sleep_for(std::chrono::seconds(6));
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-55", "-10"},
                   {"-20", "v44", "-40", "v22", "-50", "v11"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-20", "55"},
                   {"40", "v99", "10", "v66", "-20", "v44"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-55", "60"},
                   {"40", "v99", "10", "v66", "-20", "v44", "-40", "v22", "-50", "v11"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-15", "-5"}, {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "10", "10"}, {"10", "v66"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-25", "-15"}, {"-20", "v44"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-5", "-15"}, {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-45", "-55"}, {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "45", "55"}, {});

  // Test exclusive ranges.
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "(-20", "(40"}, {"10", "v66"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "(-20", "(-20"}, {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "(-20", "-10"}, {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-10", "(10"}, {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "(-50", "(-40"}, {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-55", "(11"},
                   {"10", "v66", "-20", "v44", "-40", "v22", "-50", "v11"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "(-50", "10"},
                   {"10", "v66", "-20", "v44", "-40", "v22"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "(-51", "10"},
                   {"10", "v66", "-20", "v44", "-40", "v22", "-50", "v11"});

  // Test infinity.
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-10", "+inf"},
                   {"40", "v99", "10", "v66"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-inf", "10"},
                   {"10", "v66", "-20", "v44", "-40", "v22", "-50", "v11"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-10", "(+inf"},
                   {"40", "v99", "10", "v66"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "(-inf", "10"},
                   {"10", "v66", "-20", "v44", "-40", "v22", "-50", "v11"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-inf", "+inf"},
                   {"40", "v99", "10", "v66", "-20", "v44", "-40", "v22", "-50", "v11"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "(-inf", "(+inf"},
                   {"40", "v99", "10", "v66", "-20", "v44", "-40", "v22", "-50", "v11"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "+inf", "-inf"}, {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "+inf", "10"}, {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "+inf", "+inf"}, {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "10", "-inf"}, {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "-inf", "-inf"}, {});
  SyncClient();

  // Test infinity with int64 min, max.
  DoRedisTestOk(__LINE__, {"TSADD", "ts_inf",
      int64Min_, "v1",
      "-10", "v2",
      "10", "v3",
      int64Max_, "v4",
  });
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_inf", "-inf", "+inf"},
                   {int64Max_, "v4", "10", "v3", "-10", "v2", int64Min_, "v1"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_inf", "(-inf", "(+inf"},
                   {int64Max_, "v4", "10", "v3", "-10", "v2", int64Min_, "v1"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_inf", "-inf", "-inf"},
                   {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_inf", "+inf", "+inf"},
                   {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_inf", "-10", "(+inf"},
                   {int64Max_, "v4", "10", "v3", "-10", "v2"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_inf", "-10", "+inf"},
                   {int64Max_, "v4", "10", "v3", "-10", "v2"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_inf", "(-inf", "10"},
                   {"10", "v3", "-10", "v2", int64Min_, "v1"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_inf", "-inf", "10"},
                   {"10", "v3", "-10", "v2", int64Min_, "v1"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_inf", int64Min_, int64Max_},
                   {int64Max_, "v4", "10", "v3", "-10", "v2", int64Min_, "v1"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_inf", int64MaxExclusive_, int64Max_},
                   {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_inf", int64MaxExclusive_, int64MaxExclusive_},
                   {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_inf", int64Max_, int64MaxExclusive_},
                   {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_inf", int64MinExclusive_, int64MinExclusive_},
                   {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_inf", int64MinExclusive_, int64Min_},
                   {});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_inf", int64Min_, int64MinExclusive_},
                   {} );
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_inf", int64Min_, int64Min_},
                   {int64Min_, "v1"});
  DoRedisTestArray(__LINE__, {"TSREVRANGEBYTIME", "ts_inf", int64Max_, int64Max_},
                   {int64Max_, "v4"});

  // Test invalid requests.
  DoRedisTestExpectError(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "10", "20", "30"});
  DoRedisTestExpectError(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "10", "abc"});
  DoRedisTestExpectError(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "10", "20.1"});
  DoRedisTestExpectError(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "10", "20", "LIMIT"});
  DoRedisTestExpectError(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "10", "20", "LIMIT", "BC"});
  DoRedisTestExpectError(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "10", "20", "LIMIT", "1.3"});
  DoRedisTestExpectError(__LINE__, {"TSREVRANGEBYTIME", "ts_key", "10", "20", "SOMETHING", "3"});

  DoRedisTestOk(__LINE__, {"HMSET", "map_key",
      "1", "v100",
      "2", "v200",
      "3", "v300",
      "4", "v400",
      "5", "v500",
  });
  DoRedisTestOk(__LINE__, {"HMSET", "map_key",
      "6", "v600"
  });
  DoRedisTestOk(__LINE__, {"SET", "key", "value"});
  SyncClient();
  DoRedisTestExpectError(__LINE__, {"TSREVRANGEBYTIME", "map_key", "1", "5"});
  DoRedisTestExpectError(__LINE__, {"TSREVRANGEBYTIME", "key"});

  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestTsRem) {

  // Try some deletes before inserting any data.
  DoRedisTestOk(__LINE__, {"TSREM", "invalid_key", "20", "40", "70", "90"});

  DoRedisTestOk(__LINE__, {"TSADD", "ts_key",
      "10", "v1",
      "20", "v2",
      "30", "v3",
      "40", "v4",
      "50", "v5",
      "60", "v6",
      "70", "v7",
      "80", "v8",
      "90", "v9",
      "100", "v10",
  });

  // Try some deletes.
  SyncClient();
  DoRedisTestOk(__LINE__, {"TSREM", "ts_key", "20", "40", "70", "90"});
  SyncClient();
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "10", "100"},
      {"10", "v1", "30", "v3", "50", "v5", "60", "v6", "80", "v8", "100", "v10"});
  DoRedisTestOk(__LINE__, {"TSREM", "ts_key", "30", "60", "70", "80", "90"});
  SyncClient();
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "10", "100"},
      {"10", "v1", "50", "v5", "100", "v10"});

  // Now add some data and try some more deletes.
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key",
      "25", "v25",
      "35", "v35",
      "45", "v45",
      "55", "v55",
      "75", "v75",
      "85", "v85",
      "95", "v95",
  });
  SyncClient();
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "10", "100"},
      {"10", "v1", "25", "v25", "35", "v35", "45", "v45", "50", "v5", "55", "v55",
          "75", "v75", "85", "v85", "95", "v95", "100", "v10"});
  DoRedisTestOk(__LINE__, {"TSREM", "ts_key", "10", "25", "30", "45", "50", "65", "70", "85",
      "90"});
  SyncClient();
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "10", "100"},
      {"35", "v35", "55", "v55", "75", "v75", "95", "v95", "100", "v10"});

  // Delete top level, then add some values and verify.
  DoRedisTestInt(__LINE__, {"DEL", "ts_key"}, 1);
  SyncClient();
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key",
      "22", "v22",
      "33", "v33",
      "44", "v44",
      "55", "v55",
      "77", "v77",
      "88", "v88",
      "99", "v99",
  });
  SyncClient();
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "10", "100"},
      {"22", "v22", "33", "v33", "44", "v44", "55", "v55", "77", "v77", "88", "v88",
          "99", "v99"});

  // Now try invalid commands.
  DoRedisTestExpectError(__LINE__, {"TSREM", "ts_key"}); // Not enough arguments.
  DoRedisTestExpectError(__LINE__, {"TSREM", "ts_key", "v1", "10"}); // wrong type for timestamp.
  DoRedisTestExpectError(__LINE__, {"TSREM", "ts_key", "1.0", "10"}); // wrong type for timestamp.
  DoRedisTestExpectError(__LINE__, {"HDEL", "ts_key", "22"}); // wrong delete type.
  DoRedisTestOk(__LINE__, {"HMSET", "hkey", "10", "v1", "20", "v2"});
  DoRedisTestExpectError(__LINE__, {"TSREM", "hkey", "10", "20"}); // wrong delete type.

  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestOverwrites) {
  // The default value is true, but we explicitly set this here for clarity.
  FLAGS_emulate_redis_responses = true;

  // Test Upsert.
  DoRedisTestInt(__LINE__, {"HSET", "map_key", "subkey1", "42"}, 1);
  DoRedisTestBulkString(__LINE__, {"HGET", "map_key", "subkey1"}, "42");
  // Overwrite the same key. Using Set.
  DoRedisTestOk(__LINE__, {"SET", "map_key", "new_value"});
  DoRedisTestBulkString(__LINE__, {"GET", "map_key"}, "new_value");
  SyncClient();

  // Test NX.
  DoRedisTestOk(__LINE__, {"SET", "key", "value1", "NX"});
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "value1");
  DoRedisTestNull(__LINE__, {"SET", "key", "value2", "NX"});
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "value1");

  // Test XX.
  DoRedisTestOk(__LINE__, {"SET", "key", "value2", "XX"});
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "value2");
  DoRedisTestOk(__LINE__, {"SET", "key", "value3", "XX"});
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "value3");
  DoRedisTestNull(__LINE__, {"SET", "unknown_key", "value", "XX"});

  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestSetNX) {
  // Test Insert.
  DoRedisTestInt(__LINE__, {"SETNX", "key1", "value1"}, 1);
  DoRedisTestBulkString(__LINE__, {"GET", "key1"}, "value1");
  // Overwrite the same key. Using SetNX.
  DoRedisTestInt(__LINE__, {"SETNX", "key1", "new_value"}, 0);
  DoRedisTestBulkString(__LINE__, {"GET", "key1"}, "value1");
  // Test a new key.
  DoRedisTestInt(__LINE__, {"SETNX", "key2", "value2"}, 1);
  DoRedisTestBulkString(__LINE__, {"GET", "key2"}, "value2");

  // Test `SET key value NX`.
  DoRedisTestOk(__LINE__, {"SET", "key3", "value3", "NX"});
  DoRedisTestBulkString(__LINE__, {"GET", "key3"}, "value3");
  DoRedisTestNull(__LINE__, {"SET", "key3", "new_value", "NX"});
  DoRedisTestBulkString(__LINE__, {"GET", "key3"}, "value3");

  // Test `SET key value NX` after SETNX.
  DoRedisTestNull(__LINE__, {"SET", "key1", "new_value", "NX"});
  DoRedisTestBulkString(__LINE__, {"GET", "key1"}, "value1");

  // Test SETNX after `SET key value NX`.
  DoRedisTestInt(__LINE__, {"SETNX", "key3", "new_value"}, 0);
  DoRedisTestBulkString(__LINE__, {"GET", "key3"}, "value3");
  // Test a new key.
  DoRedisTestInt(__LINE__, {"SETNX", "key4", "value4"}, 1);
  DoRedisTestBulkString(__LINE__, {"GET", "key4"}, "value4");

  // Now try invalid commands.
  DoRedisTestExpectError(__LINE__, {"SETNX"}); // Not enough arguments.
  DoRedisTestExpectError(__LINE__, {"SETNX", "key"}); // Not enough arguments.
  DoRedisTestExpectError(__LINE__, {"SETNX", "key", "score", "value"}); // Too many arguments.
  DoRedisTestExpectError(__LINE__, {"SETNX", "key", "score1", "value1", "score2",
                                    "value2"}); // Too many arguments.

  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestAdditionalCommands) {

  // The default value is true, but we explicitly set this here for clarity.
  FLAGS_emulate_redis_responses = true;

  DoRedisTestInt(__LINE__, {"HSET", "map_key", "subkey1", "42"}, 1);
  DoRedisTestInt(__LINE__, {"HSET", "map_key", "subkey2", "12"}, 1);

  SyncClient();

  // With emulate_redis_responses flag = true, we expect an int response 0 because the subkey
  // already existed. If flag is false, we'll get an OK response, which is tested later.
  DoRedisTestInt(__LINE__, {"HSET", "map_key", "subkey1", "41"}, 0);

  SyncClient();

  DoRedisTestBulkString(__LINE__, {"HGET", "map_key", "subkey1"}, "41");

  DoRedisTestResultsArray(
      __LINE__, {"HMGET", "map_key", "subkey1", "subkey3", "subkey2"},
      {RedisReply(RedisReplyType::kString, "41"), RedisReply(),
       RedisReply(RedisReplyType::kString, "12")});

  DoRedisTestArray(__LINE__, {"HGETALL", "map_key"}, {"subkey1", "41", "subkey2", "12"});

  DoRedisTestOk(__LINE__, {"SET", "key1", "30"});

  SyncClient();

  // Should be error due to wrong type.
  DoRedisTestExpectError(__LINE__, {"HGET", "key1", "subkey1"});

  DoRedisTestBulkString(__LINE__, {"GETSET", "key1", "val1"}, "30");
  DoRedisTestNull(__LINE__, {"GETSET", "non_existent", "val2"});

  SyncClient();

  DoRedisTestBulkString(__LINE__, {"GET", "key1"}, "val1");
  DoRedisTestInt(__LINE__, {"APPEND", "key1", "extra1"}, 10);

  SyncClient();

  DoRedisTestBulkString(__LINE__, {"GET", "key1"}, "val1extra1");

  DoRedisTestNull(__LINE__, {"GET", "key2"});
  // Deleting an empty key should return 0
  DoRedisTestInt(__LINE__, {"DEL", "key2"}, 0);
  // Appending to an empty key should work
  DoRedisTestInt(__LINE__, {"APPEND", "key2", "val2"}, 4);

  SyncClient();

  DoRedisTestBulkString(__LINE__, {"GET", "key2"}, "val2");

  SyncClient();

  DoRedisTestInt(__LINE__, {"DEL", "key2"}, 1);

  SyncClient();

  DoRedisTestNull(__LINE__, {"GET", "key2"});
  DoRedisTestInt(__LINE__, {"SETRANGE", "key1", "2", "xyz3"}, 10);
  DoRedisTestInt(__LINE__, {"SETRANGE", "sr1", "2", "abcd"}, 6);
  DoRedisTestBulkString(__LINE__, {"GET", "sr1"}, "\0\0abcd"s);

  SyncClient();

  DoRedisTestBulkString(__LINE__, {"GET", "key1"}, "vaxyz3tra1");
  DoRedisTestOk(__LINE__, {"SET", "key3", "23"});

  SyncClient();

  DoRedisTestInt(__LINE__, {"INCR", "key3"}, 24);
  // If no value is present, 0 is the default
  DoRedisTestInt(__LINE__, {"INCR", "key4"}, 1);

  SyncClient();

  DoRedisTestBulkString(__LINE__, {"GET", "key3"}, "24");

  DoRedisTestInt(__LINE__, {"STRLEN", "key1"}, 10);
  DoRedisTestInt(__LINE__, {"STRLEN", "key2"}, 0);
  DoRedisTestInt(__LINE__, {"STRLEN", "key3"}, 2);

  DoRedisTestInt(__LINE__, {"EXISTS", "key1"}, 1);
  DoRedisTestInt(__LINE__, {"EXISTS", "key2"}, 0);
  DoRedisTestInt(__LINE__, {"EXISTS", "key3"}, 1);
  DoRedisTestInt(__LINE__, {"EXISTS", "map_key"}, 1);
  DoRedisTestBulkString(__LINE__, {"GETRANGE", "key1", "1", "-1"}, "axyz3tra1");
  DoRedisTestBulkString(__LINE__, {"GETRANGE", "key5", "1", "4"}, "");
  DoRedisTestBulkString(__LINE__, {"GETRANGE", "key1", "-12", "4"}, "vaxyz");

  DoRedisTestOk(__LINE__, {"HMSET", "map_key", "subkey5", "19", "subkey6", "14"});
  // The last value for a duplicate key is picked up.
  DoRedisTestOk(__LINE__, {"HMSET", "map_key", "hashkey1", "v1", "hashkey2", "v2",
      "hashkey1", "v3"});

  SyncClient();

  DoRedisTestArray(__LINE__, {"HGETALL", "map_key"},
      {"hashkey1", "v3", "hashkey2", "v2", "subkey1", "41", "subkey2", "12", "subkey5", "19",
          "subkey6", "14"});
  DoRedisTestArray(__LINE__, {"HKEYS", "map_key"},
      {"hashkey1", "hashkey2", "subkey1", "subkey2", "subkey5", "subkey6"});
  DoRedisTestArray(__LINE__, {"HVALS", "map_key"},
      {"v3", "v2", "41", "12", "19", "14"});
  DoRedisTestInt(__LINE__, {"HLEN", "map_key"}, 6);
  DoRedisTestInt(__LINE__, {"HLEN", "does_not_exist"}, 0);
  DoRedisTestInt(__LINE__, {"HEXISTS", "map_key", "subkey1"}, 1);
  DoRedisTestInt(__LINE__, {"HEXISTS", "map_key", "subkey2"}, 1);
  DoRedisTestInt(__LINE__, {"HEXISTS", "map_key", "subkey3"}, 0);
  DoRedisTestInt(__LINE__, {"HEXISTS", "map_key", "subkey4"}, 0);
  DoRedisTestInt(__LINE__, {"HEXISTS", "map_key", "subkey5"}, 1);
  DoRedisTestInt(__LINE__, {"HEXISTS", "map_key", "subkey6"}, 1);
  // HSTRLEN
  DoRedisTestInt(__LINE__, {"HSTRLEN", "map_key", "subkey1"}, 2);
  DoRedisTestInt(__LINE__, {"HSTRLEN", "map_key", "does_not_exist"}, 0);
  SyncClient();

  // HDEL
  // subkey7 doesn't exists
  DoRedisTestInt(__LINE__, {"HDEL", "map_key", "subkey2", "subkey7", "subkey5"}, 2);
  SyncClient();
  DoRedisTestInt(__LINE__, {"HDEL", "map_key", "subkey9"}, 0);
  SyncClient();
  DoRedisTestInt(__LINE__, {"EXISTS", "map_key"}, 1);
  DoRedisTestArray(__LINE__, {"HGETALL", "map_key"}, {"hashkey1", "v3", "hashkey2", "v2",
      "subkey1", "41", "subkey6", "14"});
  DoRedisTestInt(__LINE__, {"DEL", "map_key"}, 1); // Delete the whole map with a del
  SyncClient();

  DoRedisTestInt(__LINE__, {"EXISTS", "map_key"}, 0);
  DoRedisTestArray(__LINE__, {"HGETALL", "map_key"}, {});

  DoRedisTestInt(__LINE__, {"EXISTS", "set1"}, 0);
  DoRedisTestInt(__LINE__, {"SADD", "set1", "val1"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"DEL", "set1"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"SADD", "set1", "val1"}, 1);
  DoRedisTestInt(__LINE__, {"SADD", "set2", "val5", "val5", "val5"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"EXISTS", "set1"}, 1);

  SyncClient();

  DoRedisTestInt(__LINE__, {"SADD", "set1", "val2", "val1", "val3"}, 2);

  SyncClient();

  DoRedisTestArray(__LINE__, {"SMEMBERS", "set1"}, {"val1", "val2", "val3"});
  DoRedisTestInt(__LINE__, {"SCARD", "set1"}, 3);
  DoRedisTestInt(__LINE__, {"SCARD", "does_not_exist"}, 0);
  DoRedisTestInt(__LINE__, {"SISMEMBER", "set1", "val1"}, 1);
  DoRedisTestInt(__LINE__, {"SISMEMBER", "set1", "val2"}, 1);
  DoRedisTestInt(__LINE__, {"SISMEMBER", "set1", "val3"}, 1);
  DoRedisTestInt(__LINE__, {"SISMEMBER", "set1", "val4"}, 0);
  SyncClient();

  // SREM remove val1 and val3. val4 doesn't exist.
  DoRedisTestInt(__LINE__, {"SREM", "set1", "val1", "val3", "val4"}, 2);
  SyncClient();
  DoRedisTestArray(__LINE__, {"SMEMBERS", "set1"}, {"val2"});

  // AUTH accepts 1 argument.
  DoRedisTestExpectError(__LINE__, {"AUTH", "foo", "subkey5", "19", "subkey6", "14"});
  DoRedisTestExpectError(__LINE__, {"AUTH"});
  // CONFIG should be dummy implementations, that respond OK irrespective of the arguments
  DoRedisTestOk(__LINE__, {"CONFIG", "foo", "subkey5", "19", "subkey6", "14"});
  DoRedisTestOk(__LINE__, {"CONFIG"});
  // Commands are pipelined and only sent when client.commit() is called.
  // sync_commit() waits until all responses are received.
  SyncClient();

  DoRedisTest(__LINE__, {"ROLE"}, RedisReplyType::kArray,
      [](const RedisReply& reply) {
        const auto& replies = reply.as_array();
        ASSERT_EQ(3, replies.size());
        ASSERT_EQ("master", replies[0].as_string());
        ASSERT_EQ(0, replies[1].as_integer());
        ASSERT_TRUE(replies[2].is_array()) << "replies[2]: " << replies[2].ToString();
        ASSERT_EQ(0, replies[2].as_array().size());
      }
  );

  DoRedisTestBulkString(__LINE__, {"PING", "foo"}, "foo");
  DoRedisTestSimpleString(__LINE__, {"PING"}, "PONG");

  DoRedisTestOk(__LINE__, {"QUIT"});

  DoRedisTestOk(__LINE__, {"FLUSHDB"});

  SyncClient();

  VerifyCallbacks();
}

TEST_F(TestRedisService, TestDel) {
  // The default value is true, but we explicitly set this here for clarity.
  FLAGS_emulate_redis_responses = true;

  DoRedisTestOk(__LINE__, {"SET", "key", "value"});
  DoRedisTestInt(__LINE__, {"DEL", "key"}, 1);
  DoRedisTestInt(__LINE__, {"DEL", "key"}, 0);
  DoRedisTestInt(__LINE__, {"DEL", "non_existent"}, 0);
  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestHDel) {
  // The default value is true, but we explicitly set this here for clarity.
  FLAGS_emulate_redis_responses = true;

  DoRedisTestInt(__LINE__, {"HSET", "map_key", "subkey1", "42"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"HDEL", "map_key", "subkey1", "non_existent_1", "non_existent_2"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"HDEL", "map_key", "non_existent_1"}, 0);
  SyncClient();
  DoRedisTestInt(__LINE__, {"HDEL", "map_key", "non_existent_1", "non_existent_2"}, 0);
  SyncClient();
  DoRedisTestInt(__LINE__, {"HDEL", "map_key", "non_existent_1", "non_existent_1"}, 0);
  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestSADDBatch) {
  DoRedisTestInt(__LINE__, {"SADD", "set1", "10"}, 1);
  DoRedisTestInt(__LINE__, {"SADD", "set1", "20"}, 1);
  DoRedisTestInt(__LINE__, {"SADD", "set1", "30"}, 1);
  DoRedisTestInt(__LINE__, {"SADD", "set1", "30"}, 0);
  SyncClient();
  DoRedisTestInt(__LINE__, {"SISMEMBER", "set1", "10"}, 1);
  DoRedisTestInt(__LINE__, {"SISMEMBER", "set1", "20"}, 1);
  DoRedisTestInt(__LINE__, {"SISMEMBER", "set1", "30"}, 1);
  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestSRem) {
  // The default value is true, but we explicitly set this here for clarity.
  FLAGS_emulate_redis_responses = true;

  DoRedisTestInt(__LINE__, {"SADD", "set_key", "subkey1"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"SREM", "set_key", "subkey1", "non_existent_1", "non_existent_2"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"SREM", "set_key", "non_existent_1"}, 0);
  SyncClient();
  DoRedisTestInt(__LINE__, {"SREM", "set_key", "non_existent_1", "non_existent_2"}, 0);
  SyncClient();
  DoRedisTestInt(__LINE__, {"SREM", "set_key", "non_existent_1", "non_existent_1"}, 0);
  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestEmulateFlagFalse) {
  FLAGS_emulate_redis_responses = false;

  DoRedisTestOk(__LINE__, {"HSET", "map_key", "subkey1", "42"});

  DoRedisTestOk(__LINE__, {"SADD", "set_key", "val1", "val2", "val1"});

  DoRedisTestOk(__LINE__, {"HDEL", "map_key", "subkey1", "subkey2"});

  SyncClient();

  VerifyCallbacks();
}

TEST_F(TestRedisService, TestHMGetTiming) {
  const int num_keys = 50;
  // For small hset size will not get consistent result.
  const int size_hset = 1000;
  const int num_subkeys = 1000;
  const int num_hmgets = 10;
  const bool is_random = true;
  const bool is_serial = true; // Sequentially sync client to measure latency
  const bool test_nonexisting = true;

  auto start = std::chrono::steady_clock::now();

  for (int i = 0; i < num_keys; i++) {
    for (int j = 0; j < size_hset; j++) {
      string si = std::to_string(i);
      string sj = std::to_string(j);
      DoRedisTestInt(__LINE__, {"HSET", "parent_" + si, "subkey_" + sj, "value_" + sj}, 1);
    }
    if (IsSanitizer() || ((i & 0x7) == 0)) {
      SyncClient();
    }
  }

  SyncClient();

  auto mid = std::chrono::steady_clock::now();

  int max_query_subkey = test_nonexisting ? size_hset * 2 : size_hset;

  for (int i = 0; i < num_hmgets; i++) {
    string si = std::to_string(i % num_keys);
    vector<string> command = {"HMGET", "parent_" + si};
    vector<RedisReply> expected;
    for (int j = 0; j < num_subkeys; j++) {
      int idx = is_random ?
          RandomUniformInt(0, max_query_subkey) :
          (j * max_query_subkey) / num_subkeys;
      string sj = std::to_string(idx);
      command.push_back("subkey_" + sj);
      expected.push_back(
          idx >= size_hset ? RedisReply() : RedisReply(RedisReplyType::kString, "value_" + sj));
    }
    DoRedisTestResultsArray(__LINE__, command, expected);
    if (is_serial) {
      SyncClient();
    }
  }

  SyncClient();

  auto end = std::chrono::steady_clock::now();

  auto set_time = std::chrono::duration_cast<std::chrono::milliseconds>(mid - start).count();
  auto get_time = std::chrono::duration_cast<std::chrono::milliseconds>(end - mid).count();

  LOG(INFO) << yb::Format("Total HSET time: $0ms Total HMGET time: $1ms",  set_time, get_time);

  VerifyCallbacks();
}

TEST_F(TestRedisService, TestTtlSet) {
  std::string collection_key = "russell";
  std::string values[10] = {"the", "set", "of", "all", "sets",
                            "that", "do", "not", "contain", "themselves"};
  int card = 10;
  TestTtlSet(&collection_key, values, card);
}

TEST_F(TestRedisService, TestTtlSortedSet) {
  std::string collection_key = "sort_me_up";
  CollectionEntry values[10] = { std::make_tuple("5.4223", "insertion"),
                                 std::make_tuple("-1", "bogo"),
                                 std::make_tuple("8", "selection"),
                                 std::make_tuple("3.1415926", "heap"),
                                 std::make_tuple("2.718", "quick"),
                                 std::make_tuple("1", "merge"),
                                 std::make_tuple("9.9", "bubble"),
                                 std::make_tuple("0", "radix"),
                                 std::make_tuple("9.9", "shell"),
                                 std::make_tuple("11", "comb") };
  int card = 10;
  TestTtlSortedSet(&collection_key, values, card);
}

TEST_F(TestRedisService, TestTtlHash) {
  std::string collection_key = "hash_browns";
  CollectionEntry values[10] = { std::make_tuple("eggs", "hyperloglog"),
                                 std::make_tuple("bagel", "bloom"),
                                 std::make_tuple("ham", "quotient"),
                                 std::make_tuple("salmon", "cuckoo"),
                                 std::make_tuple("porridge", "lp_norm_sketch"),
                                 std::make_tuple("muffin", "count_sketch"),
                                 std::make_tuple("doughnut", "hopscotch"),
                                 std::make_tuple("oatmeal", "fountain_codes"),
                                 std::make_tuple("fruit", "linear_probing"),
                                 std::make_tuple("toast", "chained") };
  int card = 10;
  TestTtlHash(&collection_key, values, card);
}

TEST_F(TestRedisService, TestTtlTimeseries) {
  std::string key = "timeseries";
  DoRedisTestOk(__LINE__, {"TSADD", key, "1", "hello", "2", "how", "3", "are", "5", "you"});
  // Checking TTL on timeseries.
  DoRedisTestInt(__LINE__, {"TTL", key}, -1);
  DoRedisTestInt(__LINE__, {"PTTL", key}, -1);
  SyncClient();
  // Checking PERSIST and (P)EXPIRE do not work.
  DoRedisTestExpectError(__LINE__, {"PERSIST", key});
  DoRedisTestExpectError(__LINE__, {"EXPIRE", key, "13"});
  DoRedisTestExpectError(__LINE__, {"PEXPIRE", key, "16384"});
  SyncClient();
  // Checking SETEX turns it back into a normal key.
  DoRedisTestOk(__LINE__, {"SETEX", key, "6", "17"});
  SyncClient();
  DoRedisTestBulkString(__LINE__, {"GET", key}, "17");
  SyncClient();
  std::this_thread::sleep_for(7s);
  CheckExpired(&key);
  SyncClient();
  VerifyCallbacks();
}

// For testing commands where the value is overwritten, but TTL is not.
TEST_F(TestRedisService, TestTtlModifyNoOverwrite) {
  // TODO: when we support RENAME, it should also be added here.
  std::string k1 = "key";
  std::string k2 = "keyy";
  const int64_t millisecond_error = 500;
  // Test integer modify
  DoRedisTestOk(__LINE__, {"SET", k1, "3"});
  SyncClient();
  DoRedisTestInt(__LINE__, {"EXPIRE", k1, "14"}, 1);
  DoRedisTestInt(__LINE__, {"TTL", k1}, 14);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k1}, 14000, millisecond_error);
  DoRedisTestInt(__LINE__, {"INCR", k1}, 4);
  DoRedisTestInt(__LINE__, {"TTL", k1}, 14);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k1}, 14000, millisecond_error);
  SyncClient();
  std::this_thread::sleep_for(5s);
  DoRedisTestInt(__LINE__, {"TTL", k1}, 9);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k1}, 9000, millisecond_error);
  DoRedisTestBulkString(__LINE__, {"GET", k1}, "4");
  DoRedisTestInt(__LINE__, {"INCRBY", k1, "3"}, 7);
  DoRedisTestInt(__LINE__, {"TTL", k1}, 9);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k1}, 9000, millisecond_error);
  SyncClient();
  std::this_thread::sleep_for(4s);
  DoRedisTestBulkString(__LINE__, {"GET", k1}, "7");
  DoRedisTestInt(__LINE__, {"TTL", k1}, 5);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k1}, 5000, millisecond_error);
  SyncClient();
  std::this_thread::sleep_for(5s);
  CheckExpired(&k1);
  // Test string modify
  DoRedisTestOk(__LINE__, {"SETEX", k2, "12", "from what I've tasted of desire "});
  SyncClient();
  DoRedisTestInt(__LINE__, {"TTL", k2}, 12);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k2}, 12000, millisecond_error);
  DoRedisTestInt(__LINE__, {"APPEND", k2, "I hold with those who favor fire."}, 65);
  DoRedisTestInt(__LINE__, {"TTL", k2}, 12);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k2}, 12000, millisecond_error);
  SyncClient();
  std::this_thread::sleep_for(5s);
  DoRedisTestInt(__LINE__, {"TTL", k2}, 7);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k2}, 7000, millisecond_error);
  DoRedisTestBulkString(__LINE__, {"GET", k2}, "from what I've tasted of desire "
                        "I hold with those who favor fire.");
  SyncClient();
  std::this_thread::sleep_for(3s);
  DoRedisTestInt(__LINE__, {"SETRANGE", k2, "5", "the beginning of time, sir"}, 65);
  DoRedisTestInt(__LINE__, {"TTL", k2}, 4);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k2}, 4000, millisecond_error);
  DoRedisTestBulkString(__LINE__, {"GET", k2}, "from the beginning of time, "
                        "sir I hold with those who favor fire.");
  SyncClient();
  std::this_thread::sleep_for(2s);
  DoRedisTestInt(__LINE__, {"TTL", k2}, 2);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k2}, 2000, millisecond_error);
  DoRedisTestBulkString(__LINE__, {"GET", k2}, "from the beginning of time, "
                        "sir I hold with those who favor fire.");
  SyncClient();
  std::this_thread::sleep_for(3s);
  CheckExpired(&k2);
  // Test Persist
  DoRedisTestOk(__LINE__, {"SETEX", k1, "13", "we've been pulling out the nails that hold up"});
  DoRedisTestInt(__LINE__, {"TTL", k1}, 13);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k1}, 13000, millisecond_error);
  DoRedisTestInt(__LINE__, {"APPEND", k1, " everything you've known"}, 69);
  SyncClient();
  std::this_thread::sleep_for(5s);
  DoRedisTestInt(__LINE__, {"TTL", k1}, 8);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k1}, 8000, millisecond_error);
  DoRedisTestBulkString(__LINE__, {"GET", k1}, "we've been pulling out the nails "
                        "that hold up everything you've known");
  DoRedisTestInt(__LINE__, {"PERSIST", k1}, 1);
  DoRedisTestInt(__LINE__, {"TTL", k1}, -1);
  DoRedisTestInt(__LINE__, {"PTTL", k1}, -1);
  SyncClient();
  std::this_thread::sleep_for(9s);
  DoRedisTestBulkString(__LINE__, {"GET", k1}, "we've been pulling out the nails "
                        "that hold up everything you've known");
  DoRedisTestInt(__LINE__, {"TTL", k1}, -1);
  DoRedisTestInt(__LINE__, {"PTTL", k1}, -1);
}

// For testing TTL-related commands on primitives.
TEST_F(TestRedisService, TestTtlPrimitive) {
  std::string k1 = "foo";
  std::string k2 = "fu";
  std::string k3 = "phu";
  std::string value = "bar";
  int64_t millisecond_error = 500;
  // Checking expected behavior on a key with no ttl.
  DoRedisTestOk(__LINE__, {"SET", k1, value});
  SyncClient();
  DoRedisTestBulkString(__LINE__, {"GET", k1}, value);
  DoRedisTestInt(__LINE__, {"TTL", k2}, -2);
  DoRedisTestInt(__LINE__, {"PTTL", k2}, -2);
  DoRedisTestInt(__LINE__, {"TTL", k1}, -1);
  DoRedisTestInt(__LINE__, {"PTTL", k1}, -1);
  SyncClient();
  // Setting a TTL and checking expected return values.
  DoRedisTestInt(__LINE__, {"EXPIRE", k1, "4"}, 1);
  SyncClient();
  {
    int attempt = 1;
    while (true) {
      const auto ttl_set_at = std::chrono::system_clock::now();
      DoRedisTestInt(__LINE__, {"TTL", k1}, 4);
      DoRedisTestApproxInt(__LINE__, {"PTTL", k1}, 4000, millisecond_error);
      DoRedisTestBulkString(__LINE__, {"GET", k1}, value);
      SyncClient();
      auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now() - ttl_set_at).count();
      std::this_thread::sleep_for(std::max(
          static_cast<int64_t>(0), static_cast<int64_t>(2500 - elapsed_ms)) * 1ms);
      // By this point there should be about 1.4 seconds left until the key's expiration.
      auto total_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now() - ttl_set_at).count();
      if (total_elapsed_ms > 2550) {
        if (attempt < 10) {
          LOG(INFO) << "TTL test took too long, re-trying (attempt: "
                    << attempt << ")";
          attempt++;
          DoRedisTestOk(__LINE__, {"SET", k1, value});
          DoRedisTestInt(__LINE__, {"EXPIRE", k1, "4"}, 1);
          continue;
        } else {
          LOG(WARNING) << "TTL test took too long, not re-trying: attempt=" << attempt;
        }
      }
      DoRedisTestInt(__LINE__, {"TTL", k1}, 1);
      DoRedisTestApproxInt(__LINE__, {"PTTL", k1}, 1000, millisecond_error);
      DoRedisTestBulkString(__LINE__, {"GET", k1}, value);
      SyncClient();
      // Checking expected return values after expiration.
      std::this_thread::sleep_for(2s);
      CheckExpiredPrimitive(&k1);
      break;  // Success.
    }
  }
  // Testing functionality with SETEX.
  DoRedisTestOk(__LINE__, {"SETEX", k1, "5", value});
  SyncClient();
  DoRedisTestIntRange(__LINE__, {"TTL", k1}, 4, 5);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k1}, 4500, millisecond_error);
  DoRedisTestBulkString(__LINE__, {"GET", k1}, value);
  SyncClient();
  // Set a new, earlier expiration.
  DoRedisTestInt(__LINE__, {"EXPIRE", k1, "2"}, 1);
  DoRedisTestInt(__LINE__, {"TTL", k1}, 2);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k1}, 1500, millisecond_error);
  DoRedisTestBulkString(__LINE__, {"GET", k1}, value);
  SyncClient();
  // Check that the value expires as expected.
  std::this_thread::sleep_for(2s);
  CheckExpiredPrimitive(&k1);
  // Initialize with SET using the EX flag.
  DoRedisTestOk(__LINE__, {"SET", k1, value, "EX", "2"});
  SyncClient();
  // Set a new, later, expiration.
  DoRedisTestInt(__LINE__, {"EXPIRE", k1, "8"}, 1);
  DoRedisTestInt(__LINE__, {"TTL", k1}, 8);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k1}, 8000, millisecond_error);
  DoRedisTestBulkString(__LINE__, {"GET", k1}, value);
  SyncClient();
  // Checking expected return values after a while, before expiration.
  std::this_thread::sleep_for(4s);
  DoRedisTestInt(__LINE__, {"TTL", k1}, 4);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k1}, 4000, millisecond_error);
  DoRedisTestBulkString(__LINE__, {"GET", k1}, value);
  SyncClient();
  // Persisting the key and checking expected return values.
  DoRedisTestInt(__LINE__, {"PERSIST", k1}, 1);
  DoRedisTestInt(__LINE__, {"TTL", k1}, -1);
  DoRedisTestInt(__LINE__, {"PTTL", k1}, -1);
  DoRedisTestBulkString(__LINE__, {"GET", k1}, value);
  SyncClient();
  // Check that the key and value are still there after a while.
  std::this_thread::sleep_for(30s);
  DoRedisTestInt(__LINE__, {"TTL", k1}, -1);
  DoRedisTestInt(__LINE__, {"PTTL", k1}, -1);
  DoRedisTestBulkString(__LINE__, {"GET", k1}, value);
  SyncClient();
  // Persist a key that does not exist.
  DoRedisTestInt(__LINE__, {"PERSIST", k2}, 0);
  SyncClient();
  // Persist a key that has no TTL.
  DoRedisTestInt(__LINE__, {"PERSIST", k1}, 0);
  SyncClient();
  // Vanilla set on a key and persisting it.
  DoRedisTestOk(__LINE__, {"SET", k2, value});
  SyncClient();
  DoRedisTestInt(__LINE__, {"PERSIST", k2}, 0);
  DoRedisTestInt(__LINE__, {"TTL", k2}, -1);
  DoRedisTestInt(__LINE__, {"PTTL", k2}, -1);
  SyncClient();
  // Expiring with an invalid TTL. We do not check the minimum,
  // because any negative value leads to an immediate deletion.
  DoRedisTestExpectError(__LINE__, {"PEXPIRE", k2,
      std::to_string(kRedisMaxTtlMillis + 1)});
  DoRedisTestExpectError(__LINE__, {"EXPIRE", k2,
      std::to_string(kRedisMaxTtlMillis / MonoTime::kMillisecondsPerSecond + 1)});
  SyncClient();
  // Test that setting a zero-valued TTL properly expires the value.
  DoRedisTestInt(__LINE__, {"EXPIRE", k2, "0"}, 1);
  CheckExpiredPrimitive(&k2);
  // One more time with a negative TTL.
  DoRedisTestOk(__LINE__, {"SET", k2, value});
  SyncClient();
  DoRedisTestInt(__LINE__, {"EXPIRE", k2, "-7"}, 1);
  CheckExpiredPrimitive(&k2);
  DoRedisTestOk(__LINE__, {"SETEX", k2, "-7", value});
  CheckExpiredPrimitive(&k2);
  // Test PExpire
  DoRedisTestOk(__LINE__, {"SET", k2, value});
  SyncClient();
  DoRedisTestInt(__LINE__, {"PEXPIRE", k2, "3200"}, 1);
  DoRedisTestInt(__LINE__, {"TTL", k2}, 3);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k2}, 3200, millisecond_error);
  SyncClient();
  std::this_thread::sleep_for(1s);
  DoRedisTestInt(__LINE__, {"TTL", k2}, 2);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k2}, 2200, millisecond_error);
  SyncClient();
  std::this_thread::sleep_for(3s);
  CheckExpiredPrimitive(&k2);
  // Test PSetEx
  DoRedisTestOk(__LINE__, {"PSETEX", k3, "2300", value});
  SyncClient();
  std::this_thread::sleep_for(1s);
  DoRedisTestInt(__LINE__, {"TTL", k3}, 1);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k3}, 1300, millisecond_error);
  SyncClient();
  std::this_thread::sleep_for(2s);
  CheckExpiredPrimitive(&k3);
  VerifyCallbacks();
}

// For testing TestExpireAt
TEST_F(TestRedisService, TestExpireAt) {
  std::string k1 = "foo";
  std::string k2 = "fu";
  std::string k3 = "phu";
  std::string value = "bar";
  int64_t millisecond_error = 500;
  int64_t second_error = 1;
  DoRedisTestOk(__LINE__, {"SET", k1, value});
  SyncClient();
  DoRedisTestInt(__LINE__, {"EXPIREAT", k1, std::to_string(std::time(0) + 5)}, 1);
  SyncClient();
  DoRedisTestApproxInt(__LINE__, {"TTL", k1}, 5, second_error);
  DoRedisTestBulkString(__LINE__, {"GET", k1}, value);
  SyncClient();
  std::this_thread::sleep_for(2s);
  DoRedisTestApproxInt(__LINE__, {"TTL", k1}, 3, second_error);
  DoRedisTestBulkString(__LINE__, {"GET", k1}, value);
  SyncClient();
  // Setting a new, later expiration.
  DoRedisTestInt(__LINE__, {"EXPIREAT", k1, std::to_string(std::time(0) + 7)}, 1);
  SyncClient();
  DoRedisTestApproxInt(__LINE__, {"TTL", k1}, 7, second_error);
  DoRedisTestBulkString(__LINE__, {"GET", k1}, value);
  SyncClient();
  // Checking expected return values after expiration.
  std::this_thread::sleep_for(8s);
  CheckExpiredPrimitive(&k1);

  // Again, but with an earlier expiration.
  DoRedisTestOk(__LINE__, {"SET", k1, value});
  SyncClient();
  DoRedisTestInt(__LINE__, {"EXPIREAT", k1, std::to_string(std::time(0) + 13)}, 1);
  SyncClient();
  DoRedisTestApproxInt(__LINE__, {"TTL", k1}, 13, second_error);
  DoRedisTestBulkString(__LINE__, {"GET", k1}, value);
  SyncClient();
  // Setting a new, earlier expiration.
  DoRedisTestInt(__LINE__, {"EXPIREAT", k1, std::to_string(std::time(0) + 5)}, 1);
  SyncClient();
  DoRedisTestApproxInt(__LINE__, {"TTL", k1}, 5, second_error);
  DoRedisTestBulkString(__LINE__, {"GET", k1}, value);
  SyncClient();
  // Check that the value expires as expected.
  std::this_thread::sleep_for(6s);
  CheckExpiredPrimitive(&k1);

  // Persisting the key and checking expected return values.
  DoRedisTestOk(__LINE__, {"SET", k1, value});
  SyncClient();
  DoRedisTestInt(__LINE__, {"EXPIREAT", k1, std::to_string(std::time(0) + 3)}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"PERSIST", k1}, 1);
  DoRedisTestInt(__LINE__, {"TTL", k1}, -1);
  DoRedisTestInt(__LINE__, {"PTTL", k1}, -1);
  DoRedisTestBulkString(__LINE__, {"GET", k1}, value);
  SyncClient();
  // Check that the key and value are still there after a while.
  std::this_thread::sleep_for(30s);
  DoRedisTestInt(__LINE__, {"TTL", k1}, -1);
  DoRedisTestInt(__LINE__, {"PTTL", k1}, -1);
  DoRedisTestBulkString(__LINE__, {"GET", k1}, value);
  SyncClient();
  // Test that setting a zero-valued time properly expires the value.
  DoRedisTestInt(__LINE__, {"EXPIREAT", k1, "0"}, 1);
  CheckExpiredPrimitive(&k1);
  // One more time with a negative expiration time.
  DoRedisTestOk(__LINE__, {"SET", k2, value});
  SyncClient();
  DoRedisTestInt(__LINE__, {"EXPIREAT", k2, "-7"}, 1);
  CheckExpiredPrimitive(&k2);
  // Again with times before the current time.
  DoRedisTestOk(__LINE__, {"SET", k2, value});
  SyncClient();
  DoRedisTestInt(__LINE__, {"EXPIREAT", k2, std::to_string(std::time(0) - 3)}, 1);
  CheckExpiredPrimitive(&k2);
  // Again with the current time.
  DoRedisTestOk(__LINE__, {"SET", k2, value});
  SyncClient();
  DoRedisTestInt(__LINE__, {"EXPIREAT", k2, std::to_string(std::time(0))}, 1);
  CheckExpiredPrimitive(&k2);
  // Test PExpireAt
  DoRedisTestOk(__LINE__, {"SET", k2, value});
  SyncClient();
  DoRedisTestInt(__LINE__, {"PEXPIREAT", k2, std::to_string(std::time(0) * 1000 + 3200)}, 1);
  DoRedisTestApproxInt(__LINE__, {"TTL", k2}, 3, second_error);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k2}, 3200, 2 * millisecond_error);
  SyncClient();
  std::this_thread::sleep_for(1s);
  DoRedisTestApproxInt(__LINE__, {"TTL", k2}, 2, second_error);
  DoRedisTestApproxInt(__LINE__, {"PTTL", k2}, 2200, 2 * millisecond_error);
  SyncClient();
  std::this_thread::sleep_for(3s);
  CheckExpiredPrimitive(&k2);
  // Test ExpireAt on nonexistent key
  DoRedisTestInt(__LINE__, {"EXPIREAT", k3, std::to_string(std::time(0) + 4)}, 0);
  SyncClient();
  DoRedisTestNull(__LINE__, {"GET", k3});
  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestQuit) {
  DoRedisTestOk(__LINE__, {"SET", "key", "value"});
  DoRedisTestBulkString(__LINE__, {"GET", "key"}, "value");
  DoRedisTestInt(__LINE__, {"DEL", "key"}, 1);
  DoRedisTestOk(__LINE__, {"QUIT"});
  SyncClient();
  VerifyCallbacks();
  // Connection closed so following command fails
  DoRedisTestExpectError(__LINE__, {"SET", "key", "value"});
  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, TestFlushAll) {
  TestFlush("FLUSHALL", false);
  TestFlush("FLUSHALL", true);
}

TEST_F(TestRedisService, TestFlushDb) {
  TestFlush("FLUSHDB", false);
  TestFlush("FLUSHDB", true);
}

// Test deque functionality of the list.
TEST_F(TestRedisService, TestListBasic) {
  DoRedisTestInt(__LINE__, {"LPUSH", "letters", "florea", "elena", "dumitru"}, 3);
  DoRedisTestInt(__LINE__, {"LLEN", "letters"}, 3);
  DoRedisTestInt(__LINE__, {"LPUSH", "letters", "constantin", "barbu"}, 5);
  DoRedisTestInt(__LINE__, {"LLEN", "letters"}, 5);
  DoRedisTestBulkString(__LINE__, {"LPOP", "letters"}, "barbu");
  DoRedisTestInt(__LINE__, {"LLEN", "letters"}, 4);
  DoRedisTestInt(__LINE__, {"LPUSH", "letters", "ana"}, 5);
  DoRedisTestInt(__LINE__, {"LLEN", "letters"}, 5);
  DoRedisTestInt(__LINE__, {"RPUSH", "letters", "lazar", "maria", "nicolae"}, 8);
  DoRedisTestInt(__LINE__, {"LLEN", "letters"}, 8);
  DoRedisTestBulkString(__LINE__, {"LPOP", "letters"}, "ana");
  DoRedisTestInt(__LINE__, {"LLEN", "letters"}, 7);
  DoRedisTestBulkString(__LINE__, {"RPOP", "letters"}, "nicolae");
  DoRedisTestInt(__LINE__, {"LLEN", "letters"}, 6);
  DoRedisTestInt(__LINE__, {"RPUSH", "letters", "gheorghe", "haralambie", "ion"}, 9);
  DoRedisTestInt(__LINE__, {"LLEN", "letters"}, 9);
  DoRedisTestInt(__LINE__, {"LPUSH", "letters", "vasile", "udrea", "tudor", "sandu"}, 13);
  DoRedisTestInt(__LINE__, {"LLEN", "letters"}, 13);
  DoRedisTestInt(__LINE__, {"RPUSH", "letters", "jiu", "kilogram"}, 15);
  DoRedisTestInt(__LINE__, {"LLEN", "letters"}, 15);
  DoRedisTestBulkString(__LINE__, {"RPOP", "letters"}, "kilogram");
  DoRedisTestInt(__LINE__, {"LLEN", "letters"}, 14);
  DoRedisTestBulkString(__LINE__, {"LPOP", "letters"}, "sandu");
  DoRedisTestInt(__LINE__, {"LLEN", "letters"}, 13);
  DoRedisTestInt(__LINE__, {"RPUSH", "letters", "dublu v", "xenia", "i grec"}, 16);
  DoRedisTestInt(__LINE__, {"LLEN", "letters"}, 16);
  DoRedisTestInt(__LINE__, {"LPUSH", "letters", "radu", "q", "petre", "olga"}, 20);
  DoRedisTestInt(__LINE__, {"LLEN", "letters"}, 20);
  DoRedisTestBulkString(__LINE__, {"RPOP", "letters"}, "i grec");
  DoRedisTestInt(__LINE__, {"LLEN", "letters"}, 19);
  DoRedisTestInt(__LINE__, {"RPUSH", "letters", "zamfir"}, 20);
  SyncClient();

  // Degenerate cases
  DoRedisTestOk(__LINE__, {"SET", "bravo", "alpha"});
  DoRedisTestNull(__LINE__, {"LPOP", "november"});
  DoRedisTestNull(__LINE__, {"RPOP", "kilo"});
  DoRedisTestInt(__LINE__, {"LPUSH", "sierra", "yankee"}, 1);
  SyncClient();
  DoRedisTestExpectError(__LINE__, {"LPOP", "bravo"});
  DoRedisTestBulkString(__LINE__, {"RPOP", "sierra"}, "yankee");
  DoRedisTestExpectError(__LINE__, {"RPOP", "bravo"});
  SyncClient();
  DoRedisTestNull(__LINE__, {"LPOP", "sierra"});
  DoRedisTestNull(__LINE__, {"RPOP", "sierra"});
}

TEST_F(TestRedisService, Keys) {
  // The default value is true, but we explicitly set this here for clarity.
  FLAGS_emulate_redis_responses = true;
  DoRedisTestInt(__LINE__, {"ZADD", "z_key_0", "1", "a"}, 1);
  DoRedisTestInt(__LINE__, {"ZADD", "z_key_0", "2", "b"}, 1);
  DoRedisTestOk(__LINE__, {"SET", "z_key_1", "v1"});
  DoRedisTestOk(__LINE__, {"SET", "z_key_1", "v2"});
  SyncClient();

  DoRedisTestArray(__LINE__, {"KEYS", "*"}, {"z_key_1", "z_key_0"});
  DoRedisTestArray(__LINE__, {"KEYS", "*key*1"}, {"z_key_1"});
  DoRedisTestArray(__LINE__, {"KEYS", "*key\\*1"}, {});
  DoRedisTestArray(__LINE__, {"KEYS", "z_key_[^1]"}, {"z_key_0"});
  DoRedisTestArray(__LINE__, {"KEYS", "z_key_[02]"}, {"z_key_0"});
  DoRedisTestArray(__LINE__, {"KEYS", "z_k?y_?"}, {"z_key_1", "z_key_0"});
  DoRedisTestArray(__LINE__, {"KEYS", "z_key_\\?"}, {});
  DoRedisTestArray(__LINE__, {"KEYS", "z_key_["}, {});
  DoRedisTestArray(__LINE__, {"KEYS", "z_key_."}, {});
  DoRedisTestArray(__LINE__, {"KEYS", "z_key_[]"}, {});
  SyncClient();

  DoRedisTestInt(__LINE__, {"HSET", "z_key_\0", "f", "v"}, 1);
  SyncClient();
  DoRedisTestArray(__LINE__, {"KEYS", "z_key_\0"}, {"z_key_\0"});

  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, KeysZeroChar) {
  FLAGS_emulate_redis_responses = true;
  string s("foo\0bar", 6);
  string s1("foo\0bars", 7);
  DoRedisTestInt(__LINE__, {"HSET", s, "1", "a"}, 1);
  SyncClient();
  DoRedisTestArray(__LINE__, {"KEYS", "foo"}, {});
  DoRedisTestArray(__LINE__, {"KEYS", s}, {s});
  DoRedisTestArray(__LINE__, {"KEYS", s1}, {});
  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, RangeScanTimeout) {
  // Test SortedSets.
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "1.0", "v1"}, 1);
  SyncClient();
  DoRedisTestArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "-inf", "+inf"}, {"v1"});
  SyncClient();

  FLAGS_TEST_tserver_timeout = true;
  DoRedisTestExpectError(__LINE__, {"ZRANGEBYSCORE", "z_key", "-inf", "+inf"},
                         "Deadline for query passed.");
  SyncClient();
  FLAGS_TEST_tserver_timeout = false;

  // Test TimeSeries.
  DoRedisTestOk(__LINE__, {"TSADD", "ts_key", "1", "v1"});
  SyncClient();
  DoRedisTestArray(__LINE__, {"TSRANGEBYTIME", "ts_key", "-inf", "+inf"}, {"1", "v1"});
  SyncClient();

  FLAGS_TEST_tserver_timeout = true;
  DoRedisTestExpectError(__LINE__, {"TSRANGEBYTIME", "ts_key", "-inf", "+inf"},
                         "Deadline for query passed.");
  SyncClient();
  FLAGS_TEST_tserver_timeout = false;

  // Test a point read doesn't time out.
  DoRedisTestOk(__LINE__, {"SET", "k", "v"});
  SyncClient();
  DoRedisTestBulkString(__LINE__, {"GET", "k"}, "v");
  SyncClient();

  FLAGS_TEST_tserver_timeout = true;
  DoRedisTestBulkString(__LINE__, {"GET", "k"}, "v");

  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, KeysTimeout) {
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "1.0", "v1"}, 1);
  SyncClient();
  FLAGS_TEST_tserver_timeout = true;
  DoRedisTestExpectError(__LINE__, {"KEYS", "*"},
                         "Errors occured while reaching out to the tablet servers");
  SyncClient();
  FLAGS_TEST_tserver_timeout = false;
  DoRedisTestArray(__LINE__, {"KEYS", "*"}, {"z_key"});
  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, KeysWithFlush) {
  for (int i = 0; i < 2; i++) {
    DoRedisTestOk(__LINE__, {"SET", Format("k$0", i), Format("v$0", i)});
    SyncClient();
    ASSERT_OK(FlushRedisTable());
  }

  DoRedisTestArray(__LINE__, {"KEYS", "*"}, {"k0", "k1"});
  SyncClient();
  VerifyCallbacks();
}

TEST_F(TestRedisService, SortedSetsIncr) {
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "1", "v1"}, 1);
  SyncClient();
  DoRedisTestInt(__LINE__, {"ZADD", "z_key", "incr", "1", "v1"}, 0);
  SyncClient();
  DoRedisTestScoreValueArray(__LINE__, {"ZRANGEBYSCORE", "z_key", "-inf", "+inf", "WITHSCORES"},
                                       {2}, {"v1"});
  SyncClient();
}

}  // namespace redisserver
}  // namespace yb
