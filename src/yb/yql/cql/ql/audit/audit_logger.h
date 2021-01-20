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
// Responsible for logging audit records in YCQL.
// Audit is controlled through gflags. If the audit is not enabled, logging methods return
// immediately without imposing overhead.
//--------------------------------------------------------------------------------------------------

#ifndef YB_YQL_CQL_QL_AUDIT_AUDIT_LOGGER_H_
#define YB_YQL_CQL_QL_AUDIT_AUDIT_LOGGER_H_

#include "yb/common/common.pb.h"
#include "yb/yql/cql/cqlserver/cql_message.h"
#include "yb/yql/cql/ql/exec/exec_context.h"

namespace yb {
namespace ql {
namespace audit {

class Type;
struct LogEntry;

class AuditLogger {
 public:
  explicit AuditLogger(const QLEnv& ql_env);

  // Sets a connection for the current (new) user operation, resetting the rescheduled mark.
  void SetConnection(const std::shared_ptr<const rpc::Connection>& conn) {
    conn_ = conn;
    rescheduled_ = false;
  }

  // Marks a current execution as being rescheduled. This will suppress non-erroneous statement
  // execution logging, and is reset by SetConnection().
  void MarkRescheduled() {
    rescheduled_ = true;
  }

  // Enters the batch request mode, should be called when driver-level batch is received.
  // This generates a UUID to identify the current batch in audit.
  //
  // Note that this is only used for batch requests, not for explicit START TRANSACTION commands
  // because in that case separate commands might arrive to different tservers.
  //
  // If this returns non-OK status, batch mode isn't activated.
  CHECKED_STATUS StartBatchRequest(int statements_count);

  // Exits the batch request mode. Does nothing outside of a batch request.
  CHECKED_STATUS EndBatchRequest();

  // Log the response to a user's authentication request.
  CHECKED_STATUS LogAuthResponse(const cqlserver::CQLResponse& response);

  // Log the statement execution start.
  // tnode might be nullptr, in which case this does nothing.
  CHECKED_STATUS LogStatement(const TreeNode* tnode,
                              const std::string& statement,
                              bool is_prepare);

  // Log the statement analysis/execution failure.
  // tnode might be nullptr, in which case this does nothing.
  CHECKED_STATUS LogStatementError(const TreeNode* tnode,
                                   const std::string& statement,
                                   const Status& error_status,
                                   bool error_is_formatted);

  // Log a general statement processing failure.
  // We should only use this directly when the parse tree is not present.
  CHECKED_STATUS LogStatementError(const std::string& statement,
                                   const Status& error_status,
                                   bool error_is_formatted);

 private:
  using GflagName = std::string;
  using GflagStringValue = std::string;
  using GflagListValue = std::unordered_set<std::string>;
  using GflagsCache = std::unordered_map<GflagName, std::pair<GflagStringValue, GflagListValue>>;

  // Whether the execution is being retried.
  std::atomic<bool> rescheduled_{false};

  const QLEnv& ql_env_;

  // Currently audited connection.
  std::shared_ptr<const rpc::Connection> conn_;

  // Empty string means not in a batch processing mode.
  // TODO(alex,mihnea): Look into potential races on this as well, see GH issue #5922.
  std::string batch_id_;

  // Cache of parsed gflags, to avoid re-parsing unchanged values.
  GflagsCache gflags_cache_;

  // Checks whether a given predicate holds on the comma-separated list gflag.
  // This uses gflag library helper to access a gflag by name, to avoid concurrently accessing
  // string gflags that may change at runtime.
  template<class Pred>
  bool SatisfiesGFlag(const LogEntry& e,
                      const std::string& gflag_name,
                      const Pred& predicate);

  // Determine whether this entry should be logged given current audit configuration.
  // Note that we reevaluate gflags to allow changing them dynamically.
  bool ShouldBeLogged(const LogEntry& e);

  Result<LogEntry> CreateLogEntry(const Type& type,
                                  std::string keyspace,
                                  std::string scope,
                                  std::string operation,
                                  std::string error_message);
};

} // namespace audit
} // namespace ql
} // namespace yb


#endif // YB_YQL_CQL_QL_AUDIT_AUDIT_LOGGER_H_
