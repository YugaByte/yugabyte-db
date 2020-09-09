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
//--------------------------------------------------------------------------------------------------

#include "yb/yql/cql/ql/ql_processor.h"

#include <memory>

#include "yb/common/roles_permissions.h"
#include "yb/client/table.h"
#include "yb/client/yb_table_name.h"

#include "yb/util/scope_exit.h"
#include "yb/util/thread_restrictions.h"

#include "yb/yql/cql/ql/statement.h"

DECLARE_bool(use_cassandra_authentication);
DECLARE_bool(ycql_require_drop_privs_for_truncate);

METRIC_DEFINE_histogram_with_percentiles(
    server, handler_latency_yb_cqlserver_SQLProcessor_ParseRequest,
    "Time spent parsing the SQL query", yb::MetricUnit::kMicroseconds,
    "Time spent parsing the SQL query", 60000000LU, 2);
METRIC_DEFINE_histogram_with_percentiles(
    server, handler_latency_yb_cqlserver_SQLProcessor_AnalyzeRequest,
    "Time spent to analyze the parsed SQL query", yb::MetricUnit::kMicroseconds,
    "Time spent to analyze the parsed SQL query", 60000000LU, 2);
METRIC_DEFINE_histogram_with_percentiles(
    server, handler_latency_yb_cqlserver_SQLProcessor_ExecuteRequest,
    "Time spent executing the parsed SQL query", yb::MetricUnit::kMicroseconds,
    "Time spent executing the parsed SQL query", 60000000LU, 2);
METRIC_DEFINE_histogram_with_percentiles(
    server, handler_latency_yb_cqlserver_SQLProcessor_NumRoundsToAnalyze,
    "Number of rounds to successfully parse a SQL query", yb::MetricUnit::kOperations,
    "Number of rounds to successfully parse a SQL query", 60000000LU, 2);
METRIC_DEFINE_histogram_with_percentiles(
    server, handler_latency_yb_cqlserver_SQLProcessor_NumRetriesToExecute,
    "Number of retries to successfully execute a SQL query", yb::MetricUnit::kOperations,
    "Number of retries to successfully execute a SQL query", 60000000LU, 2);
METRIC_DEFINE_histogram_with_percentiles(
    server, handler_latency_yb_cqlserver_SQLProcessor_NumFlushesToExecute,
    "Number of flushes to successfully execute a SQL query", yb::MetricUnit::kOperations,
    "Number of flushes to successfully execute a SQL query", 60000000LU, 2);
METRIC_DEFINE_histogram_with_percentiles(
    server, handler_latency_yb_cqlserver_SQLProcessor_SelectStmt,
    "Time spent processing a SELECT statement", yb::MetricUnit::kMicroseconds,
    "Time spent processing a SELECT statement", 60000000LU, 2);
METRIC_DEFINE_histogram_with_percentiles(
    server, handler_latency_yb_cqlserver_SQLProcessor_InsertStmt,
    "Time spent processing an INSERT statement", yb::MetricUnit::kMicroseconds,
    "Time spent processing an INSERT statement", 60000000LU, 2);
METRIC_DEFINE_histogram_with_percentiles(
    server, handler_latency_yb_cqlserver_SQLProcessor_UpdateStmt,
    "Time spent processing an UPDATE statement", yb::MetricUnit::kMicroseconds,
    "Time spent processing an UPDATE statement", 60000000LU, 2);
METRIC_DEFINE_histogram_with_percentiles(
    server, handler_latency_yb_cqlserver_SQLProcessor_DeleteStmt,
    "Time spent processing a DELETE statement", yb::MetricUnit::kMicroseconds,
    "Time spent processing a DELETE statement", 60000000LU, 2);
METRIC_DEFINE_histogram_with_percentiles(
    server, handler_latency_yb_cqlserver_SQLProcessor_OtherStmts,
    "Time spent processing any statement other than SELECT/INSERT/UPDATE/DELETE",
    yb::MetricUnit::kMicroseconds,
    "Time spent processing any statement other than SELECT/INSERT/UPDATE/DELETE", 60000000LU, 2);
METRIC_DEFINE_histogram_with_percentiles(
    server, handler_latency_yb_cqlserver_SQLProcessor_Transaction,
    "Time spent processing a transaction", yb::MetricUnit::kMicroseconds,
    "Time spent processing a transaction", 60000000LU, 2);
METRIC_DEFINE_histogram_with_percentiles(
    server, handler_latency_yb_cqlserver_SQLProcessor_ResponseSize,
    "Size of the returned response blob (in bytes)", yb::MetricUnit::kBytes,
    "Size of the returned response blob (in bytes)", 60000000LU, 2);

namespace yb {
namespace ql {

using std::shared_ptr;
using std::string;
using client::YBClient;
using client::YBMetaDataCache;
using client::YBTableName;

QLMetrics::QLMetrics(const scoped_refptr<yb::MetricEntity> &metric_entity) {
  time_to_parse_ql_query_ =
      METRIC_handler_latency_yb_cqlserver_SQLProcessor_ParseRequest.Instantiate(metric_entity);
  time_to_analyze_ql_query_ =
      METRIC_handler_latency_yb_cqlserver_SQLProcessor_AnalyzeRequest.Instantiate(metric_entity);
  time_to_execute_ql_query_ =
      METRIC_handler_latency_yb_cqlserver_SQLProcessor_ExecuteRequest.Instantiate(metric_entity);
  num_rounds_to_analyze_ql_ =
      METRIC_handler_latency_yb_cqlserver_SQLProcessor_NumRoundsToAnalyze.Instantiate(
          metric_entity);
  num_retries_to_execute_ql_ =
      METRIC_handler_latency_yb_cqlserver_SQLProcessor_NumRetriesToExecute.Instantiate(
          metric_entity);
  num_flushes_to_execute_ql_ =
      METRIC_handler_latency_yb_cqlserver_SQLProcessor_NumFlushesToExecute.Instantiate(
          metric_entity);

  ql_select_ =
      METRIC_handler_latency_yb_cqlserver_SQLProcessor_SelectStmt.Instantiate(metric_entity);
  ql_insert_ =
      METRIC_handler_latency_yb_cqlserver_SQLProcessor_InsertStmt.Instantiate(metric_entity);
  ql_update_ =
      METRIC_handler_latency_yb_cqlserver_SQLProcessor_UpdateStmt.Instantiate(metric_entity);
  ql_delete_ =
      METRIC_handler_latency_yb_cqlserver_SQLProcessor_DeleteStmt.Instantiate(metric_entity);
  ql_others_ =
      METRIC_handler_latency_yb_cqlserver_SQLProcessor_OtherStmts.Instantiate(metric_entity);
  ql_transaction_ =
      METRIC_handler_latency_yb_cqlserver_SQLProcessor_Transaction.Instantiate(metric_entity);

  ql_response_size_bytes_ =
      METRIC_handler_latency_yb_cqlserver_SQLProcessor_ResponseSize.Instantiate(metric_entity);
}

namespace {

ThreadSafeObjectPool<Parser> default_parser_pool;

}

QLProcessor::QLProcessor(client::YBClient* client,
                         shared_ptr<YBMetaDataCache> cache, QLMetrics* ql_metrics,
                         ThreadSafeObjectPool<Parser>* parser_pool,
                         const server::ClockPtr& clock,
                         TransactionPoolProvider transaction_pool_provider)
    : ql_env_(client, cache, clock, std::move(transaction_pool_provider)),
      analyzer_(&ql_env_),
      executor_(&ql_env_, this, ql_metrics),
      ql_metrics_(ql_metrics),
      parser_pool_(parser_pool ? parser_pool : &default_parser_pool) {
}

QLProcessor::~QLProcessor() {
}

Status QLProcessor::Parse(const string& stmt, ParseTree::UniPtr* parse_tree,
                          const bool reparsed, const MemTrackerPtr& mem_tracker,
                          const bool internal) {
  // Parse the statement and get the generated parse tree.
  const MonoTime begin_time = MonoTime::Now();
  auto* parser = parser_pool_->Take();
  auto scope_exit = ScopeExit([this, parser] {
    this->parser_pool_->Release(parser);
  });
  RETURN_NOT_OK(parser->Parse(stmt, reparsed, mem_tracker, internal));
  const MonoTime end_time = MonoTime::Now();
  if (ql_metrics_ != nullptr) {
    const MonoDelta elapsed_time = end_time.GetDeltaSince(begin_time);
    ql_metrics_->time_to_parse_ql_query_->Increment(elapsed_time.ToMicroseconds());
  }
  *parse_tree = parser->Done();
  DCHECK(*parse_tree) << "Parse tree is null";
  return Status::OK();
}

Status QLProcessor::Analyze(ParseTree::UniPtr* parse_tree) {
  // Semantic analysis - traverse, error-check, and decorate the parse tree nodes with datatypes.
  const MonoTime begin_time = MonoTime::Now();
  const Status s = analyzer_.Analyze(std::move(*parse_tree));
  const MonoTime end_time = MonoTime::Now();
  if (ql_metrics_ != nullptr) {
    const MonoDelta elapsed_time = end_time.GetDeltaSince(begin_time);
    ql_metrics_->time_to_analyze_ql_query_->Increment(elapsed_time.ToMicroseconds());
    ql_metrics_->num_rounds_to_analyze_ql_->Increment(1);
  }
  *parse_tree = analyzer_.Done();
  DCHECK(*parse_tree) << "Parse tree is null";
  return s;
}

Status QLProcessor::Prepare(const string& stmt, ParseTree::UniPtr* parse_tree,
                            const bool reparsed, const MemTrackerPtr& mem_tracker,
                            const bool internal) {
  RETURN_NOT_OK(Parse(stmt, parse_tree, reparsed, mem_tracker, internal));
  const Status s = Analyze(parse_tree);
  if (s.IsQLError() && GetErrorCode(s) == ErrorCode::STALE_METADATA && !reparsed) {
    *parse_tree = nullptr;
    RETURN_NOT_OK(Parse(stmt, parse_tree, true /* reparsed */, mem_tracker));
    return Analyze(parse_tree);
  }
  return s;
}

bool QLProcessor::CheckPermissions(const ParseTree& parse_tree, StatementExecutedCallback cb) {
  const TreeNode* tnode = parse_tree.root().get();
  if (tnode != nullptr) {
    Status s = CheckNodePermissions(tnode);
    if (!s.ok()) {
      cb.Run(s, nullptr);
      return false;
    }
  }
  return true;
}

Status QLProcessor::CheckNodePermissions(const TreeNode* tnode) {
  Status s; // OK by default initialization.
  switch (DCHECK_NOTNULL(tnode)->opcode()) {
    case TreeNodeOpcode::kPTCreateKeyspace:
      s = ql_env_.HasResourcePermission("data", OBJECT_SCHEMA, PermissionType::CREATE_PERMISSION);
      break;
    case TreeNodeOpcode::kPTCreateTable: {
      const char* keyspace =
          static_cast<const PTCreateTable*>(tnode)->table_name()->first_name().c_str();
      s = ql_env_.HasResourcePermission(get_canonical_keyspace(keyspace), OBJECT_SCHEMA,
                                        PermissionType::CREATE_PERMISSION, keyspace);
      break;
    }
    case TreeNodeOpcode::kPTCreateType:
      // Check has AllKeyspaces permission.
      s = ql_env_.HasResourcePermission("data", OBJECT_SCHEMA, PermissionType::CREATE_PERMISSION);
      if (!s.ok()) {
        const string keyspace =
            static_cast<const PTCreateType*>(tnode)->yb_type_name().namespace_name();
        // Check has Keyspace permission.
        s = ql_env_.HasResourcePermission(get_canonical_keyspace(keyspace),
            OBJECT_SCHEMA, PermissionType::CREATE_PERMISSION, keyspace);
      }
      break;
    case TreeNodeOpcode::kPTCreateIndex: {
      const YBTableName indexed_table_name =
          static_cast<const PTCreateIndex*>(tnode)->indexed_table_name();
      s = ql_env_.HasTablePermission(indexed_table_name, PermissionType::ALTER_PERMISSION);
      break;
    }
    case TreeNodeOpcode::kPTAlterTable: {
      const YBTableName table_name = static_cast<const PTAlterTable*>(tnode)->yb_table_name();
      s = ql_env_.HasTablePermission(table_name, PermissionType::ALTER_PERMISSION);
      break;
    }
    case TreeNodeOpcode::kPTTruncateStmt: {
      const YBTableName table_name = static_cast<const PTTruncateStmt*>(tnode)->yb_table_name();
      if (FLAGS_ycql_require_drop_privs_for_truncate) {
        s = ql_env_.HasTablePermission(table_name, PermissionType::DROP_PERMISSION);
      } else {
        s = ql_env_.HasTablePermission(table_name, PermissionType::MODIFY_PERMISSION);
      }
      break;
    }
    case TreeNodeOpcode::kPTExplainStmt: {
      const PTExplainStmt* explain_stmt = static_cast<const PTExplainStmt*>(tnode);
      s = CheckNodePermissions(DCHECK_NOTNULL(explain_stmt->stmt().get()));
      break;
    }
    case TreeNodeOpcode::kPTUpdateStmt: FALLTHROUGH_INTENDED;
    case TreeNodeOpcode::kPTDeleteStmt: FALLTHROUGH_INTENDED;
    case TreeNodeOpcode::kPTInsertStmt: {
      DCHECK(tnode->IsDml());
      const YBTableName table_name = static_cast<const PTDmlStmt*>(tnode)->table_name();
      s = ql_env_.HasTablePermission(table_name, PermissionType::MODIFY_PERMISSION);
      break;
    }
    case TreeNodeOpcode::kPTSelectStmt: {
      const auto select_stmt = static_cast<const PTSelectStmt*>(tnode);
      if (select_stmt->IsReadableByAllSystemTable()) {
        break;
      }
      const YBTableName table_name = select_stmt->table_name();
      s = ql_env_.HasTablePermission(table_name, PermissionType::SELECT_PERMISSION);
      break;
    }
    case TreeNodeOpcode::kPTCreateRole:
      s = ql_env_.HasResourcePermission("roles", ObjectType::OBJECT_ROLE,
                                        PermissionType::CREATE_PERMISSION);
      break;
    case TreeNodeOpcode::kPTAlterRole: {
      const char* role = static_cast<const PTAlterRole*>(tnode)->role_name();
      s = ql_env_.HasRolePermission(role, PermissionType::ALTER_PERMISSION);
      break;
    }
    case TreeNodeOpcode::kPTGrantRevokeRole: {
      const auto grant_revoke_role_stmt = static_cast<const PTGrantRevokeRole*>(tnode);
      const string granted_role = grant_revoke_role_stmt->granted_role_name();
      const string recipient_role = grant_revoke_role_stmt->recipient_role_name();
      s = ql_env_.HasRolePermission(granted_role, PermissionType::AUTHORIZE_PERMISSION);
      if (s.ok()) {
        s = ql_env_.HasRolePermission(recipient_role, PermissionType::AUTHORIZE_PERMISSION);
      }
      break;
    }
    case TreeNodeOpcode::kPTGrantRevokePermission: {
      const auto grant_revoke_permission = static_cast<const PTGrantRevokePermission*>(tnode);
      const string canonical_resource = grant_revoke_permission->canonical_resource();
      const char* keyspace = grant_revoke_permission->namespace_name();
      // It's only a table name if the resource type is TABLE.
      const char* table = grant_revoke_permission->resource_name();
      switch (grant_revoke_permission->resource_type()) {
        case ResourceType::KEYSPACE: {
          DCHECK_EQ(canonical_resource, get_canonical_keyspace(keyspace));
          s = ql_env_.HasResourcePermission(canonical_resource, OBJECT_SCHEMA,
                                            PermissionType::AUTHORIZE_PERMISSION,
                                            keyspace);
          break;
        }
        case ResourceType::TABLE: {
          DCHECK_EQ(canonical_resource, get_canonical_table(keyspace, table));
          s = ql_env_.HasTablePermission(keyspace, table, PermissionType::AUTHORIZE_PERMISSION);
          break;
        }
        case ResourceType::ROLE: {
          DCHECK_EQ(canonical_resource,
                    get_canonical_role(grant_revoke_permission->resource_name()));
          s = ql_env_.HasResourcePermission(canonical_resource, OBJECT_ROLE,
                                            PermissionType::AUTHORIZE_PERMISSION);
          break;
        }
        case ResourceType::ALL_KEYSPACES: {
          DCHECK_EQ(canonical_resource, "data");
          s = ql_env_.HasResourcePermission(canonical_resource, OBJECT_SCHEMA,
                                            PermissionType::AUTHORIZE_PERMISSION);
          break;
        }
        case ResourceType::ALL_ROLES:
          DCHECK_EQ(canonical_resource, "roles");
          s = ql_env_.HasResourcePermission(canonical_resource, OBJECT_ROLE,
                                            PermissionType::AUTHORIZE_PERMISSION);
          break;
      }
      break;
    }
    case TreeNodeOpcode::kPTDropStmt: {
      const auto drop_stmt = static_cast<const PTDropStmt*>(tnode);
      const ObjectType object_type = drop_stmt->drop_type();
      switch(object_type) {
        case OBJECT_ROLE:
          s = ql_env_.HasRolePermission(drop_stmt->name()->QLName(),
                                        PermissionType::DROP_PERMISSION);
          break;
        case OBJECT_SCHEMA:
          s = ql_env_.HasResourcePermission(
              get_canonical_keyspace(drop_stmt->yb_table_name().namespace_name()),
              OBJECT_SCHEMA, PermissionType::DROP_PERMISSION);
          break;
        case OBJECT_TABLE:
          s = ql_env_.HasTablePermission(drop_stmt->yb_table_name(),
                                         PermissionType::DROP_PERMISSION);
          break;
        case OBJECT_TYPE:
          // Check has AllKeyspaces permission.
          s = ql_env_.HasResourcePermission(
              "data", OBJECT_SCHEMA, PermissionType::DROP_PERMISSION);
          if (!s.ok()) {
            const string keyspace = drop_stmt->yb_table_name().namespace_name();
            // Check has Keyspace permission.
            s = ql_env_.HasResourcePermission(get_canonical_keyspace(keyspace),
                OBJECT_SCHEMA, PermissionType::DROP_PERMISSION, keyspace);
          }
          break;
        case OBJECT_INDEX: {
          bool cache_used = false;
          const YBTableName table_name(YQL_DATABASE_CQL,
                                       drop_stmt->yb_table_name().namespace_name(),
                                       drop_stmt->yb_table_name().table_name());
          std::shared_ptr<client::YBTable> table = ql_env_.GetTableDesc(table_name, &cache_used);

          // If the table is not found, or if it's not an index, let the operation go through
          // so that we can return a "not found" error.s
          if (table && table->IsIndex()) {
            std::shared_ptr<client::YBTable> indexed_table =
                ql_env_.GetTableDesc(table->index_info().indexed_table_id(), &cache_used);

            if (!indexed_table) {
              s = STATUS_SUBSTITUTE(InternalError,
                                    "Unable to find index $0",
                                    drop_stmt->name()->QLName());
              break;
            }

            s = ql_env_.HasTablePermission(indexed_table->name(),
                                           PermissionType::ALTER_PERMISSION);
          }
          break;
        }
        default:
          break;
      }
      break;
    }
    default:
      break;
  }
  return s;
}

void QLProcessor::ExecuteAsync(const ParseTree& parse_tree, const StatementParameters& params,
                               StatementExecutedCallback cb) {
  if (FLAGS_use_cassandra_authentication && !parse_tree.internal()) {
    if (!CheckPermissions(parse_tree, cb)) {
      return;
    }
  }
  executor_.ExecuteAsync(parse_tree, params, std::move(cb));
}

void QLProcessor::ExecuteAsync(const StatementBatch& batch, StatementExecutedCallback cb) {
  executor_.ExecuteAsync(batch, std::move(cb));
}

void QLProcessor::RunAsync(const string& stmt, const StatementParameters& params,
                           StatementExecutedCallback cb, const bool reparsed) {
  ParseTree::UniPtr parse_tree;
  const Status s = Prepare(stmt, &parse_tree, reparsed);
  if (PREDICT_FALSE(!s.ok())) {
    return cb.Run(s, nullptr /* result */);
  }
  const ParseTree* ptree = parse_tree.release();
  // Do not make a copy of stmt and params when binding to the RunAsyncDone callback because when
  // error occurs due to stale matadata, the statement needs to be reexecuted. We should pass the
  // original references which are guaranteed to still be alive when the statement is reexecuted.
  ExecuteAsync(*ptree, params, Bind(&QLProcessor::RunAsyncDone, Unretained(this), ConstRef(stmt),
                                    ConstRef(params), Owned(ptree), cb));
}

void QLProcessor::RunAsyncDone(const string& stmt, const StatementParameters& params,
                               const ParseTree* parse_tree, StatementExecutedCallback cb,
                               const Status& s, const ExecutedResult::SharedPtr& result) {
  // If execution fails due to stale metadata and the statement has not been reparsed, rerun this
  // statement with stale metadata flushed. The rerun needs to be rescheduled in because this
  // callback may not be executed in the RPC worker thread. Also, rescheduling gives other calls a
  // chance to execute first before we do.
  if (s.IsQLError() && GetErrorCode(s) == ErrorCode::STALE_METADATA && !parse_tree->reparsed()) {
    return Reschedule(&run_async_task_.Bind(this, stmt, params, std::move(cb)));
  }
  cb.Run(s, result);
}

void QLProcessor::Reschedule(rpc::ThreadPoolTask* task) {
  // Some unit tests are not executed in CQL proxy. In those cases, just execute the callback
  // directly while disabling thread restrictions.
  const bool allowed = ThreadRestrictions::SetWaitAllowed(true);
  task->Run();
  // In such tests QLProcessor is deleted right after Run is executed, since QLProcessor tasks
  // do nothing in Done we could just don't execute it.
  // task->Done(Status::OK());
  ThreadRestrictions::SetWaitAllowed(allowed);
}

}  // namespace ql
}  // namespace yb
