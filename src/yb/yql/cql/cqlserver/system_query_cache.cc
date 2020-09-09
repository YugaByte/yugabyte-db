//--------------------------------------------------------------------------------------------------
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
// SystemQueryCache proactively caches system queries to be used by
// CQLProcessors.  This helps with performance and availability, since masters
// have reduced traffic when a connection is established.
//--------------------------------------------------------------------------------------------------

#include "yb/yql/cql/cqlserver/system_query_cache.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

#include <boost/optional.hpp>

#include "yb/rpc/io_thread_pool.h"
#include "yb/rpc/scheduler.h"
#include "yb/util/async_util.h"
#include "yb/util/format.h"
#include "yb/util/monotime.h"
#include "yb/util/string_util.h"
#include "yb/yql/cql/cqlserver/cql_service.h"
#include "yb/yql/cql/ql/util/statement_params.h"
#include "yb/yql/cql/ql/util/statement_result.h"

typedef struct QualifiedTable {
  std::string keyspace;
  std::string table;

  QualifiedTable(std::string keyspace_, std::string table_) : keyspace(keyspace_), table(table_) {
  }
} QualifiedTable;

static bool parse_tables(
    const std::string& text,
    std::vector<QualifiedTable>* tables) {
  auto raw_tables = yb::StringSplit(text, ';');
  std::remove_if(raw_tables.begin(), raw_tables.end(),
      [](const std::string& s) { return s.length() == 0; });

  for (const auto& raw : raw_tables) {
    auto table_pair = yb::StringSplit(raw, '.');

    if (table_pair.size() != 2) {
      return false;
    }
    tables->push_back(QualifiedTable(table_pair[0], table_pair[1]));
  }

  return true;
}

static bool validate_tables(const char* flagname, const std::string& value) {
  std::vector<QualifiedTable> tables;

  if (parse_tables(value, &tables)) {
    return true;
  }
  printf("Invalid value for --%s: %s\n", flagname, value.c_str());
  return false;
}

DEFINE_int32(cql_update_system_query_cache_msecs, 0,
             "How often the system query cache should be updated. <= 0 disables caching.");
DEFINE_int32(cql_system_query_cache_stale_msecs, 60000,
             "Maximum permitted staleness for the system query cache. "
             "<= 0 permits infinite staleness.");
DEFINE_string(cql_system_query_cache_tables, "",
    "Tables to cache connection data for. Entries are semicolon-delimited, in the "
    "format <keyspace>.<table>.");
__attribute__((unused))
DEFINE_validator(cql_system_query_cache_tables, &validate_tables);
DEFINE_bool(cql_system_query_cache_empty_responses, true,
            "Whether to cache empty responses from the master.");

namespace yb {
namespace cqlserver {

using ql::RowsResult;
using ql::ExecutedResult;

// TODO: Possibly do a case-insensitive string comparison.  This may be easier
// said than done, since capitalization does matter for comparisons in WHERE
// clauses, etc.
const char* SYSTEM_QUERIES[] = {
  "SELECT * FROM system.peers",
  "SELECT peer, rpc_address, schema_version FROM system.peers",
  "SELECT peer, data_center, rack, release_version, rpc_address FROM system.peers",
  "SELECT keyspace_name, table_name, start_key, end_key, replica_addresses FROM system.partitions",

  "SELECT * FROM system.local WHERE key='local'",
  "SELECT schema_version FROM system.local WHERE key='local'",
  // The client evidently doesn't always have consistent formatting.
  "select * from system.local where key = 'local'",
  "select schema_version from system.local where key='local'",
  "SELECT * FROM system_schema.keyspaces",
  "SELECT * FROM system_schema.tables",
  "SELECT * FROM system_schema.views",
  "SELECT * FROM system_schema.columns",
  "SELECT * FROM system_schema.types",
  "SELECT * FROM system_schema.functions",
  "SELECT * FROM system_schema.aggregates",
  "SELECT * FROM system_schema.triggers",
  "SELECT * FROM system_schema.indexes",
};

SystemQueryCache::SystemQueryCache(cqlserver::CQLServiceImpl* service_impl)
  : service_impl_(service_impl), stmt_params_() {

  cache_ = std::make_unique<std::unordered_map<std::string, RowsResult::SharedPtr>>();
  last_updated_ = MonoTime::kMin;
  InitializeQueries();

  pool_ = std::make_unique<yb::rpc::IoThreadPool>("system_query_cache_updater", 1);
  scheduler_ = std::make_unique<yb::rpc::Scheduler>(&pool_->io_service());

  LOG(INFO) << "Created system query cache updater.";

  if (FLAGS_cql_update_system_query_cache_msecs > 0) {
    if (MonoDelta::FromMilliseconds(FLAGS_cql_system_query_cache_stale_msecs) <
        MonoDelta::FromMilliseconds(FLAGS_cql_update_system_query_cache_msecs)) {
      LOG(WARNING) << "Stale expiration shorter than update rate.";
    }

    ScheduleRefreshCache(false /* now */);
  } else {
    LOG(WARNING) << "System cache created with nonpositive timeout. Disabling scheduling";
  }
}

SystemQueryCache::~SystemQueryCache() {
  if (pool_) {
    scheduler_->Shutdown();
    pool_->Shutdown();
    pool_->Join();
  }
}

void SystemQueryCache::InitializeQueries() {
  for (auto query : SYSTEM_QUERIES) {
    queries_.push_back(query);
  }

  std::vector<QualifiedTable> table_pairs;
  // This should have been caught by the flag validator
  if (!parse_tables(FLAGS_cql_system_query_cache_tables, &table_pairs)) {
    return;
  }

  const char* formats[] = {
    "SELECT * FROM system_schema.tables WHERE keyspace_name = '$0' AND table_name = '$1'",
    "SELECT * FROM system_schema.columns WHERE keyspace_name = '$0' AND table_name = '$1'",
    "SELECT * FROM system_schema.triggers WHERE keyspace_name = '$0' AND table_name = '$1'",
    "SELECT * FROM system_schema.indexes WHERE keyspace_name = '$0' AND table_name = '$1'",
    "SELECT * FROM system_schema.views WHERE keyspace_name = '$0' AND view_name = '$1'",
  };

  for (auto pair : table_pairs) {
    for (auto format : formats) {
      queries_.push_back(yb::Format(format, pair.keyspace, pair.table));
    }
  }

}

boost::optional<RowsResult::SharedPtr> SystemQueryCache::Lookup(const std::string& query) {
  if (FLAGS_cql_system_query_cache_stale_msecs > 0 &&
      GetStaleness() > MonoDelta::FromMilliseconds(FLAGS_cql_system_query_cache_stale_msecs)) {
    return boost::none;
  }
  const std::lock_guard<std::mutex> l(cache_mutex_);

  const auto it = cache_->find(query);
  if (it == cache_->end()) {
    return boost::none;
  } else {
    return it->second;
  }
}

MonoDelta SystemQueryCache::GetStaleness() {
  const std::lock_guard<std::mutex> l(cache_mutex_);
  return MonoTime::Now() - last_updated_;
}

void SystemQueryCache::RefreshCache() {
  VLOG(1) << "Refreshing system query cache";
  auto new_cache = std::make_unique<std::unordered_map<std::string, RowsResult::SharedPtr>>();
  for (auto query : queries_) {
    Status status;
    ExecutedResult::SharedPtr result;
    ExecuteSync(query, &status, &result);

    if (status.ok()) {
      auto rows_result = std::dynamic_pointer_cast<RowsResult>(result);
      if (FLAGS_cql_system_query_cache_empty_responses ||
          rows_result->GetRowBlock()->row_count() > 0) {
        (*new_cache)[query] = rows_result;
      } else {
        LOG(INFO) << "Skipping empty result for statement: " << query;
      }
    } else {
      LOG(WARNING) << "Could not execute statement: " << query;
      // We don't want to update the cache with no data; instead we'll let the
      // stale cache persist.
      ScheduleRefreshCache(false /* now */);
      return;
    }
  }

  {
    const std::lock_guard<std::mutex> l(cache_mutex_);
    cache_ = std::move(new_cache);
    last_updated_ = MonoTime::Now();
  }

  ScheduleRefreshCache(false /* now */);
}

void SystemQueryCache::ScheduleRefreshCache(bool now) {
  DCHECK(pool_);
  DCHECK(scheduler_);
  VLOG(1) << "Scheduling cache refresh";

  scheduler_->Schedule([this](const Status &s) {
      if (!s.ok()) {
        LOG(INFO) << "System cache updater scheduler was shutdown: " << s.ToString();
        return;
      }
      this->RefreshCache();
      }, std::chrono::milliseconds(now ? 0 : FLAGS_cql_update_system_query_cache_msecs));
}

void SystemQueryCache::ExecuteSync(const std::string& stmt, Status* status,
    ExecutedResult::SharedPtr* result_ptr) {
  const auto processor = service_impl_->GetProcessor();
  if (!processor.ok()) {
    LOG(ERROR) << "Unable to get CQLProcessor for system query cache";
    *status = processor.status();
    return;
  }

  Synchronizer sync;
  const auto callback = [](Synchronizer* sync, ExecutedResult::SharedPtr* result_ptr,
      const Status& status, const ExecutedResult::SharedPtr& result) {
    *result_ptr = result;
    sync->StatusCB(status);
  };

  (*processor)->RunAsync(stmt, stmt_params_, yb::Bind(+callback, &sync, result_ptr));
  *status = sync.Wait();
  (*processor)->Release();
}

} // namespace cqlserver
} // namespace yb
