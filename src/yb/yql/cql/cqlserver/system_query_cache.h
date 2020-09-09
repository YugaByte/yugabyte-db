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

#ifndef YB_YQL_CQL_CQLSERVER_SYSTEM_QUERY_CACHE_H_
#define YB_YQL_CQL_CQLSERVER_SYSTEM_QUERY_CACHE_H_

#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <boost/optional.hpp>

#include "yb/gutil/thread_annotations.h"
#include "yb/rpc/io_thread_pool.h"
#include "yb/rpc/scheduler.h"
#include "yb/util/monotime.h"
#include "yb/yql/cql/ql/util/statement_params.h"
#include "yb/yql/cql/ql/util/statement_result.h"

namespace chrono = std::chrono;

namespace yb {

namespace cqlserver {
class CQLServiceImpl;
}

namespace cqlserver {

using ql::RowsResult;
using ql::ExecutedResult;

class SystemQueryCache {
 public:
    explicit SystemQueryCache(cqlserver::CQLServiceImpl* service_impl);
    ~SystemQueryCache();

    boost::optional<RowsResult::SharedPtr> Lookup(const std::string& query);

    MonoDelta GetStaleness();

 private:
    void InitializeQueries();
    void RefreshCache();
    void ScheduleRefreshCache(bool now);
    void ExecuteSync(const std::string& stmt, Status* status,
        ExecutedResult::SharedPtr* result_ptr);

    cqlserver::CQLServiceImpl* const service_impl_;
    std::vector<std::string> queries_;

    std::unique_ptr<std::unordered_map<std::string, RowsResult::SharedPtr>> cache_
      GUARDED_BY(cache_mutex_);
    MonoTime last_updated_ GUARDED_BY(cache_mutex_);
    std::mutex cache_mutex_;

    // Required for executing statements
    ql::StatementParameters stmt_params_;

    // Thread pool used by the scheduler.
    std::unique_ptr<yb::rpc::IoThreadPool> pool_;
    // The scheduler used to refresh the system queries.
    std::unique_ptr<yb::rpc::Scheduler> scheduler_;
};

} // namespace cqlserver
} // namespace yb
#endif  // YB_YQL_CQL_CQLSERVER_SYSTEM_QUERY_CACHE_H_
