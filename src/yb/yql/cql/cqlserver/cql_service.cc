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

#include "yb/yql/cql/cqlserver/cql_service.h"

#include <openssl/sha.h>

#include <mutex>
#include <thread>

#include <boost/compute/detail/lru_cache.hpp>

#include "yb/client/meta_data_cache.h"
#include "yb/client/transaction_pool.h"

#include "yb/gutil/strings/join.h"

#include "yb/yql/cql/cqlserver/cql_processor.h"
#include "yb/yql/cql/cqlserver/cql_rpc.h"
#include "yb/yql/cql/cqlserver/cql_server.h"
#include "yb/yql/cql/cqlserver/system_query_cache.h"

#include "yb/gutil/strings/substitute.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/rpc_context.h"
#include "yb/tserver/tablet_server.h"

#include "yb/util/bytes_formatter.h"
#include "yb/util/crypt.h"

#include "yb/util/mem_tracker.h"

using namespace std::placeholders;
using namespace yb::size_literals;

DECLARE_bool(use_cassandra_authentication);
DECLARE_int32(cql_update_system_query_cache_msecs);

DEFINE_int64(cql_service_max_prepared_statement_size_bytes, 128_MB,
             "The maximum amount of memory the CQL proxy should use to maintain prepared "
             "statements. 0 or negative means unlimited.");
DEFINE_int32(cql_ybclient_reactor_threads, 24,
             "The number of reactor threads to be used for processing ybclient "
             "requests originating in the cql layer");
DEFINE_int32(password_hash_cache_size, 64, "Number of password hashes to cache. 0 or "
             "negative disables caching.");
DEFINE_int64(cql_processors_limit, -4000,
             "Limit number of CQL processors. Positive means absolute limit. "
             "Negative means number of processors per 1GB of root mem tracker memory limit. "
             "0 - unlimited.");

namespace yb {
namespace cqlserver {

const char* const kRoleColumnNameSaltedHash = "salted_hash";
const char* const kRoleColumnNameCanLogin = "can_login";

using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;
using strings::Substitute;
using yb::client::YBSchema;
using yb::client::YBSession;
using yb::client::YBMetaDataCache;
using yb::rpc::InboundCall;

class ParserFactory {
 public:
  explicit ParserFactory(CQLMetrics* metrics) : metrics_(metrics) {}

  ql::Parser* operator()() const {
    metrics_->parsers_created_->Increment();
    metrics_->parsers_alive_->Increment();
    return new ql::Parser;
  }

 private:
  CQLMetrics* metrics_;
};

class ParserDeleter {
 public:
  explicit ParserDeleter(CQLMetrics* metrics) : metrics_(metrics) {}

  void operator()(ql::Parser* parser) const {
    metrics_->parsers_alive_->Decrement();
    delete parser;
  }

 private:
  CQLMetrics* metrics_;
};

int64_t CQLProcessorsLimit() {
  auto value = FLAGS_cql_processors_limit;
  if (value > 0) {
    return value;
  }
  if (value == 0) {
    return std::numeric_limits<int64_t>::max();
  }
  return (-value * MemTracker::GetRootTracker()->limit()) >> 30;
}

CQLServiceImpl::CQLServiceImpl(CQLServer* server, const CQLServerOptions& opts)
    : CQLServerServiceIf(server->metric_entity()),
      server_(server),
      next_available_processor_(processors_.end()),
      password_cache_(FLAGS_password_hash_cache_size),
      // TODO(ENG-446): Handle metrics for all the methods individually.
      cql_metrics_(std::make_shared<CQLMetrics>(server->metric_entity())),
      parser_pool_(ParserFactory(cql_metrics_.get()), ParserDeleter(cql_metrics_.get())),
      messenger_(server->messenger()) {

  // Setup prepared statements' memory tracker. Add garbage-collect function to delete least
  // recently used statements when limit is hit.
  prepared_stmts_mem_tracker_ = MemTracker::CreateTracker(
      FLAGS_cql_service_max_prepared_statement_size_bytes > 0 ?
      FLAGS_cql_service_max_prepared_statement_size_bytes : -1,
      "CQL prepared statements", server->mem_tracker());

  LOG(INFO) << "CQL processors limit: " << CQLProcessorsLimit();

  processors_mem_tracker_ = MemTracker::CreateTracker("CQL processors", server->mem_tracker());

  auth_prepared_stmt_ = std::make_shared<ql::Statement>(
      "",
      Substitute("SELECT $0, $1 FROM system_auth.roles WHERE role = ?",
                 kRoleColumnNameSaltedHash, kRoleColumnNameCanLogin));

  if (FLAGS_cql_update_system_query_cache_msecs > 0) {
    system_cache_ = std::make_shared<SystemQueryCache>(this);
  } else {
    VLOG(1) << "System query cache disabled.";
  }
}

CQLServiceImpl::~CQLServiceImpl() {
}

client::YBClient* CQLServiceImpl::client() const {
  auto client = server_->tserver()->client();
  if (client && !is_metadata_initialized_.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> l(metadata_init_mutex_);
    if (!is_metadata_initialized_.load(std::memory_order_acquire)) {
      // Create and save the metadata cache object.
      metadata_cache_ = std::make_shared<YBMetaDataCache>(client,
                                                          FLAGS_use_cassandra_authentication);
      is_metadata_initialized_.store(true, std::memory_order_release);
    }
  }
  return client;
}

const std::shared_ptr<client::YBMetaDataCache>& CQLServiceImpl::metadata_cache() const {
  // Call client to wait for client and initialize metadata_cache if not already done.
  (void)client();
  return metadata_cache_;
}

void CQLServiceImpl::CompleteInit() {
  prepared_stmts_mem_tracker_->AddGarbageCollector(shared_from_this());
}

void CQLServiceImpl::Shutdown() {
  decltype(processors_) processors;
  {
    std::lock_guard<std::mutex> guard(processors_mutex_);
    processors.swap(processors_);
  }
  for (const auto& processor : processors) {
    processor->Shutdown();
  }

  auto client = this->client();
  if (client) {
    client->messenger()->Shutdown();
  }
}

void CQLServiceImpl::Handle(yb::rpc::InboundCallPtr inbound_call) {
  TRACE("Handling the CQL call");
  // Collect the call.
  CQLInboundCall* cql_call = down_cast<CQLInboundCall*>(CHECK_NOTNULL(inbound_call.get()));
  DVLOG(4) << "Handling " << cql_call->ToString();

  // Process the call.
  MonoTime start = MonoTime::Now();

  Result<CQLProcessor*> processor = GetProcessor();
  if (!processor.ok()) {
    inbound_call->RespondFailure(rpc::ErrorStatusPB::ERROR_SERVER_TOO_BUSY, processor.status());
    return;
  }
  MonoTime got_processor = MonoTime::Now();
  cql_metrics_->time_to_get_cql_processor_->Increment(
      got_processor.GetDeltaSince(start).ToMicroseconds());
  (**processor).ProcessCall(std::move(inbound_call));
}

Result<CQLProcessor*> CQLServiceImpl::GetProcessor() {
  CQLProcessorListPos pos;
  {
    // Retrieve the next available processor. If none is available, allocate a new slot in the list.
    // Then create the processor outside the mutex below.
    std::lock_guard<std::mutex> guard(processors_mutex_);
    if (next_available_processor_ != processors_.end()) {
      return (next_available_processor_++)->get();
    }

    auto limit = CQLProcessorsLimit();
    if (num_allocated_processors_ >= limit) {
      return STATUS_FORMAT(ServiceUnavailable,
                           "Unable to allocate CQL processor, already allocated $0 of $1",
                           num_allocated_processors_, limit);
    }
    ++num_allocated_processors_;
    pos = processors_.emplace(processors_.end());
  }

  *pos = std::make_unique<CQLProcessor>(this, pos);
  return pos->get();
}

void CQLServiceImpl::ReturnProcessor(const CQLProcessorListPos& pos) {
  // Put the processor back before the next available one.
  std::lock_guard<std::mutex> guard(processors_mutex_);
  processors_.splice(next_available_processor_, processors_, pos);
  next_available_processor_ = pos;
}

shared_ptr<CQLStatement> CQLServiceImpl::AllocatePreparedStatement(
    const CQLMessage::QueryId& query_id, const string& keyspace, const string& query) {
  // Get exclusive lock before allocating a prepared statement and updating the LRU list.
  std::lock_guard<std::mutex> guard(prepared_stmts_mutex_);

  shared_ptr<CQLStatement> stmt;
  const auto itr = prepared_stmts_map_.find(query_id);
  if (itr == prepared_stmts_map_.end()) {
    // Allocate the prepared statement placeholder that multiple clients trying to prepare the same
    // statement to contend on. The statement will then be prepared by one client while the rest
    // wait for the results.
    stmt = prepared_stmts_map_.emplace(
        query_id, std::make_shared<CQLStatement>(
            keyspace, query, prepared_stmts_list_.end())).first->second;
    InsertLruPreparedStatementUnlocked(stmt);
  } else {
    // Return existing statement if found.
    stmt = itr->second;
    MoveLruPreparedStatementUnlocked(stmt);
  }

  VLOG(1) << "InsertPreparedStatement: CQL prepared statement cache count = "
          << prepared_stmts_map_.size() << "/" << prepared_stmts_list_.size()
          << ", memory usage = " << prepared_stmts_mem_tracker_->consumption();

  return stmt;
}

shared_ptr<const CQLStatement> CQLServiceImpl::GetPreparedStatement(
    const CQLMessage::QueryId& query_id) {
  // Get exclusive lock before looking up a prepared statement and updating the LRU list.
  std::lock_guard<std::mutex> guard(prepared_stmts_mutex_);

  const auto itr = prepared_stmts_map_.find(query_id);
  if (itr == prepared_stmts_map_.end()) {
    return nullptr;
  }

  shared_ptr<CQLStatement> stmt = itr->second;

  // If the statement has not finished preparing, do not return it.
  if (stmt->unprepared()) {
    return nullptr;
  }
  // If the statement is stale, delete it.
  if (stmt->stale()) {
    DeletePreparedStatementUnlocked(stmt);
    return nullptr;
  }

  MoveLruPreparedStatementUnlocked(stmt);
  return stmt;
}

void CQLServiceImpl::DeletePreparedStatement(const shared_ptr<const CQLStatement>& stmt) {
  // Get exclusive lock before deleting the prepared statement.
  std::lock_guard<std::mutex> guard(prepared_stmts_mutex_);

  DeletePreparedStatementUnlocked(stmt);

  VLOG(1) << "DeletePreparedStatement: CQL prepared statement cache count = "
          << prepared_stmts_map_.size() << "/" << prepared_stmts_list_.size()
          << ", memory usage = " << prepared_stmts_mem_tracker_->consumption();
}

bool CQLServiceImpl::CheckPassword(
    const std::string plain,
    const std::string expected_bcrypt_hash) {
  if (FLAGS_password_hash_cache_size <= 0) {
    return util::bcrypt_checkpw(plain.c_str(), expected_bcrypt_hash.c_str()) == 0;
  }

  std::string sha_hash(SHA256_DIGEST_LENGTH, '\0');
  {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, plain.c_str(), plain.length());
    SHA256_Final((unsigned char*) &sha_hash[0], &ctx);
  }
  // bcrypt can generate multiple hashes from a single key, since a salt is
  // randomly generated each time a password is set. Using a compound key allows
  // the same plaintext to be associated with different hashes.
  std::string key = sha_hash + ":" + expected_bcrypt_hash;

  {
    std::lock_guard<std::mutex> guard(password_cache_mutex_);
    auto entry = password_cache_.get(key);
    if (entry) {
      return true;
    }
  }

  // bcrypt_checkpw has stringcmp semantics.
  bool correct = util::bcrypt_checkpw(plain.c_str(), expected_bcrypt_hash.c_str()) == 0;
  if (correct) {
    std::lock_guard<std::mutex> guard(password_cache_mutex_);
    // Boost's LRU cache interprets insertion of a duplicate key as a no-op, so
    // even if two threads successfully log in to the same account, there should
    // not be a race condition here.
    password_cache_.insert(key, true);
  }
  return correct;
}

void CQLServiceImpl::InsertLruPreparedStatementUnlocked(const shared_ptr<CQLStatement>& stmt) {
  // Insert the statement at the front of the LRU list.
  stmt->set_pos(prepared_stmts_list_.insert(prepared_stmts_list_.begin(), stmt));
}

void CQLServiceImpl::MoveLruPreparedStatementUnlocked(const shared_ptr<CQLStatement>& stmt) {
  // Move the statement to the front of the LRU list.
  prepared_stmts_list_.splice(prepared_stmts_list_.begin(), prepared_stmts_list_, stmt->pos());
}

void CQLServiceImpl::DeletePreparedStatementUnlocked(
    const std::shared_ptr<const CQLStatement> stmt) {
  // Remove statement from cache by looking it up by query ID and only when it is same statement
  // object. Note that the "stmt" parameter above is not a ref ("&") intentionally so that we have
  // a separate copy of the shared_ptr and not the very shared_ptr in prepared_stmts_map_ or
  // prepared_stmt_list_ we are deleting.
  const auto itr = prepared_stmts_map_.find(stmt->query_id());
  if (itr != prepared_stmts_map_.end() && itr->second == stmt) {
    prepared_stmts_map_.erase(itr);
  }
  // Remove statement from LRU list only when it is in the list, i.e. pos() != end().
  if (stmt->pos() != prepared_stmts_list_.end()) {
    prepared_stmts_list_.erase(stmt->pos());
    stmt->set_pos(prepared_stmts_list_.end());
  }
}

void CQLServiceImpl::CollectGarbage(size_t required) {
  // Get exclusive lock before deleting the least recently used statement at the end of the LRU
  // list from the cache.
  std::lock_guard<std::mutex> guard(prepared_stmts_mutex_);

  if (!prepared_stmts_list_.empty()) {
    DeletePreparedStatementUnlocked(prepared_stmts_list_.back());
  }

  VLOG(1) << "DeleteLruPreparedStatement: CQL prepared statement cache count = "
          << prepared_stmts_map_.size() << "/" << prepared_stmts_list_.size()
          << ", memory usage = " << prepared_stmts_mem_tracker_->consumption();
}

client::TransactionPool* CQLServiceImpl::TransactionPool() {
  return server_->tserver()->TransactionPool();
}

server::Clock* CQLServiceImpl::clock() {
  return server_->clock();
}

}  // namespace cqlserver
}  // namespace yb
