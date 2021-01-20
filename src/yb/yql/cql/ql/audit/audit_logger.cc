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
//--------------------------------------------------------------------------------------------------

#include "yb/yql/cql/ql/audit/audit_logger.h"

#include <boost/algorithm/string.hpp>
#include <boost/optional/optional_io.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/random_generator.hpp>

#include "yb/rpc/connection.h"
#include "yb/util/date_time.h"
#include "yb/yql/cql/ql/ptree/pt_alter_keyspace.h"
#include "yb/yql/cql/ql/ptree/pt_alter_table.h"
#include "yb/yql/cql/ql/ptree/pt_create_index.h"
#include "yb/yql/cql/ql/ptree/pt_create_keyspace.h"
#include "yb/yql/cql/ql/ptree/pt_create_role.h"
#include "yb/yql/cql/ql/ptree/pt_create_table.h"
#include "yb/yql/cql/ql/ptree/pt_create_type.h"
#include "yb/yql/cql/ql/ptree/pt_delete.h"
#include "yb/yql/cql/ql/ptree/pt_drop.h"
#include "yb/yql/cql/ql/ptree/pt_explain.h"
#include "yb/yql/cql/ql/ptree/pt_grant_revoke.h"
#include "yb/yql/cql/ql/ptree/pt_select.h"
#include "yb/yql/cql/ql/ptree/pt_truncate.h"
#include "yb/yql/cql/ql/ptree/pt_use_keyspace.h"
#include "yb/yql/cql/ql/util/statement_result.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/flag_tags.h"
#include "yb/util/string_util.h"
#include "yb/util/type_traits.h"

DEFINE_bool(ycql_enable_audit_log,
            false,
            "Enable YCQL audit. Use ycql_audit_* flags for fine-grained configuration");
TAG_FLAG(ycql_enable_audit_log, runtime);

// IMPORTANT:
// These flags are expected to change at runtime, but due to the nature of std::string we shouldn't
// access them directly as a concurrent read-write access would lead to an undefined behaviour.
// Instead, use FLAGS_NAMESPACE::GetCommandLineOption("flag_name", &result)

DEFINE_string(ycql_audit_log_level,
              "ERROR",
              "Severity level at which an audit will be logged. Could be INFO, WARNING, or ERROR");
TAG_FLAG(ycql_audit_log_level, runtime);

DEFINE_string(ycql_audit_included_keyspaces,
              "",
              "Comma separated list of keyspaces to be included in the audit log, "
              "if none - includes all keyspaces");
TAG_FLAG(ycql_audit_included_keyspaces, runtime);

DEFINE_string(ycql_audit_excluded_keyspaces,
              "system,system_schema,system_virtual_schema,system_auth",
              "Comma separated list of keyspaces to be excluded from the audit log");
TAG_FLAG(ycql_audit_excluded_keyspaces, runtime);

DEFINE_string(ycql_audit_included_categories,
              "",
              "Comma separated list of categories to be included in the audit log, "
              "if none - includes all categories");
TAG_FLAG(ycql_audit_included_categories, runtime);

DEFINE_string(ycql_audit_excluded_categories,
              "",
              "Comma separated list of categories to be excluded from the audit log");
TAG_FLAG(ycql_audit_excluded_categories, runtime);

DEFINE_string(ycql_audit_included_users,
              "",
              "Comma separated list of users to be included in the audit log");
TAG_FLAG(ycql_audit_included_users, runtime);

DEFINE_string(ycql_audit_excluded_users,
              "",
              "Comma separated list of users to be excluded from the audit log");
TAG_FLAG(ycql_audit_excluded_users, runtime);


namespace yb {
namespace ql {
namespace audit {

using boost::optional;
using yb::cqlserver::CQLMessage;
using yb::cqlserver::CQLResponse;
using yb::cqlserver::ErrorResponse;

//
// Entities
//

namespace {

// Audit categories are modelled after Apache Cassandra 4.0-beta2.
YB_DEFINE_ENUM(Category, (QUERY)(DML)(DDL)(DCL)(AUTH)(PREPARE)(ERROR)(OTHER))

} // anonymous namespace

// Audit types are modelled after Apache Cassandra 4.0-beta2.
#define YCQL_AUDIT_TYPES \
    ((SELECT, QUERY)) \
    ((UPDATE, DML)) /* Includes INSERT. */ \
    ((DELETE, DML)) \
    ((BATCH, DML)) \
    \
    /* DDL */ \
    \
    ((TRUNCATE, DDL)) \
    ((CREATE_KEYSPACE, DDL)) \
    ((ALTER_KEYSPACE, DDL)) \
    ((DROP_KEYSPACE, DDL)) \
    ((CREATE_TABLE, DDL)) \
    ((DROP_TABLE, DDL)) \
    ((CREATE_INDEX, DDL)) \
    ((DROP_INDEX, DDL)) \
    ((CREATE_TYPE, DDL)) \
    ((ALTER_TABLE, DDL)) \
    ((DROP_TYPE, DDL)) \
    ((ALTER_TYPE, DDL)) \
    \
    /* DCL */ \
    \
    ((GRANT, DCL)) \
    ((REVOKE, DCL)) \
    ((CREATE_ROLE, DCL)) \
    ((DROP_ROLE, DCL)) \
    ((ALTER_ROLE, DCL)) \
    \
    /* AUTH */ \
    \
    ((LOGIN_ERROR, AUTH)) \
    ((UNAUTHORIZED_ATTEMPT, AUTH)) \
    ((LOGIN_SUCCESS, AUTH)) \
    \
    /* Misc */ \
    \
    ((PREPARE_STATEMENT, PREPARE)) \
    ((REQUEST_FAILURE, ERROR)) \
    ((USE_KEYSPACE, OTHER)) \
    ((DESCRIBE, OTHER)) \
    ((EXPLAIN, OTHER)) \
    /**/

#define DECLARE_YCQL_AUDIT_TYPE(type_name, category_name) \
    static const Type type_name; \
    /**/

// Audit type with category, an enum-like class.
class Type {
 public:
  BOOST_PP_SEQ_FOR_EACH(YB_STATUS_FORWARD_MACRO, DECLARE_YCQL_AUDIT_TYPE, YCQL_AUDIT_TYPES)

  const std::string name_;
  const Category    category_;

 private:
  explicit Type(std::string name, Category category)
      : name_(std::move(name)), category_(category) {
  }
};

#define DEFINE_YCQL_AUDIT_TYPE(type_name, category_name) \
    const Type Type::type_name = Type(BOOST_PP_STRINGIZE(type_name), Category::category_name); \
    /**/

BOOST_PP_SEQ_FOR_EACH(YB_STATUS_FORWARD_MACRO, DEFINE_YCQL_AUDIT_TYPE, YCQL_AUDIT_TYPES)

struct LogEntry {
  // Username of the currently active user, special case is "anonymous" user. None if not logged in.
  // Note that we can't distinguish between anonymous user and user named "anonymous",
  // but that's expected.
  optional<std::string> user;

  // Host (current node) endpoint.
  const Endpoint& host;

  // Source (CQL client) endpoint.
  const Endpoint& source;

  // Node local timestamp, unrelated to transaction read point.
  const Timestamp timestamp;

  // Audit log entry type (and category). Cannot be null.
  const Type* type;

  // Batch ID, for driver-level batch requests only.
  const std::string batch_id;

  // Keyspace, if applicable, empty string if none.
  const std::string keyspace;

  // Scope, if applicable, empty string if none. Exact meaning of "scope" depends on an operation.
  const std::string scope;

  // CQL being executed, or login status.
  std::string operation;

  // Operaion error message, empty string if none.
  std::string error_message;

  std::string ToString() const {
    return Format("{ user: $0, host: $1, source: $2, timestamp: $3, type: $4, category: $5,"
                  " keyspace: $6, scope: $7, operation: $8, error_message: $9 }",
                  user, host, source, timestamp, type->name_, type->category_,
                  keyspace, scope, operation, error_message);
  }
};


//
// Local functions
//

namespace {

// * Splits a comma-separated gflag list
// * Trims each string
// * Uppercases them (using the fact that CQL's keyspaces and users are case-insensitive)
// * Filters out empty strings
std::unordered_set<std::string> SplitGflagList(const std::string& gflag) {
  std::vector<std::string> split;
  boost::split(split, gflag, boost::is_any_of(","), boost::token_compress_on);
  for (auto& s : split) {
    boost::algorithm::trim(s);
    boost::algorithm::to_upper(s);
  }
  split.erase(std::remove_if(split.begin(), split.end(), [](auto& s){ return s.empty(); }),
              split.end());
  return std::unordered_set<std::string>(split.begin(), split.end());
}

template<typename T>
bool Contains(const std::unordered_set<T>& set, const T& value) {
  return set.find(value) != set.end();
}

// Return an audit log type for a tree node, or nullptr if a node can't be audited.
// Also sets an operation keyspace and scope, if applicable, following Cassandra's behaviour.
const Type* GetAuditLogTypeOption(const TreeNode& tnode,
                                  std::string* keyspace,
                                  std::string* scope) {
  // FIXME: DESCRIBE and LIST <...> are client-only operations and are not audited!
  switch (tnode.opcode()) {
    case TreeNodeOpcode::kPTSelectStmt: {
      const auto& cast_node = static_cast<const PTSelectStmt&>(tnode);
      *keyspace = cast_node.table_name().namespace_name();
      *scope    = cast_node.table_name().table_name();
      return &Type::SELECT;
    }
    case TreeNodeOpcode::kPTInsertStmt:
    case TreeNodeOpcode::kPTUpdateStmt: {
      const auto& cast_node = static_cast<const PTDmlStmt&>(tnode);
      *keyspace = cast_node.table_name().namespace_name();
      *scope    = cast_node.table_name().table_name();
      return &Type::UPDATE;
    }
    case TreeNodeOpcode::kPTDeleteStmt: {
      const auto& cast_node = static_cast<const PTDeleteStmt&>(tnode);
      *keyspace = cast_node.table_name().namespace_name();
      *scope    = cast_node.table_name().table_name();
      return &Type::DELETE;
    }
    case TreeNodeOpcode::kPTTruncateStmt: {
      const auto& cast_node = static_cast<const PTTruncateStmt&>(tnode);
      *keyspace = cast_node.yb_table_name().namespace_name();
      *scope    = cast_node.yb_table_name().table_name();
      return &Type::TRUNCATE;
    }
    case TreeNodeOpcode::kPTCreateKeyspace: {
      const auto& cast_node = static_cast<const PTCreateKeyspace&>(tnode);
      *keyspace = cast_node.name();
      return &Type::CREATE_KEYSPACE;
    }
    case TreeNodeOpcode::kPTAlterKeyspace: {
      const auto& cast_node = static_cast<const PTAlterKeyspace&>(tnode);
      *keyspace = cast_node.name();
      return &Type::ALTER_KEYSPACE;
    }
    case TreeNodeOpcode::kPTUseKeyspace: {
      const auto& cast_node = static_cast<const PTUseKeyspace&>(tnode);
      *keyspace = cast_node.name();
      return &Type::USE_KEYSPACE;
    }
    case TreeNodeOpcode::kPTDropStmt: {
      const auto& cast_node = static_cast<const PTDropStmt&>(tnode);
      // We only expect a handful of types here, same as Executor::ExecPTNode(const PTDropStmt*)
      switch (cast_node.drop_type()) {
        case OBJECT_SCHEMA:
          *keyspace = cast_node.name()->last_name().data();
          return &Type::DROP_KEYSPACE;
        case OBJECT_INDEX:
          *keyspace = cast_node.yb_table_name().namespace_name();
          *scope    = cast_node.yb_table_name().table_name();
          return &Type::DROP_INDEX;
        case OBJECT_ROLE:
          return &Type::DROP_ROLE;
        case OBJECT_TABLE:
          *keyspace = cast_node.yb_table_name().namespace_name();
          *scope    = cast_node.yb_table_name().table_name();
          return &Type::DROP_TABLE;
        case OBJECT_TYPE:
          *keyspace = cast_node.name()->first_name().data();
          *scope    = cast_node.name()->last_name().data();
          return &Type::DROP_TYPE;
        default:
          return nullptr;
      }
    }
    case TreeNodeOpcode::kPTCreateTable: {
      const auto& cast_node = static_cast<const PTCreateTable&>(tnode);
      *keyspace = cast_node.yb_table_name().namespace_name();
      *scope    = cast_node.yb_table_name().table_name();
      return &Type::CREATE_TABLE;
    }
    case TreeNodeOpcode::kPTCreateIndex: {
      const auto& cast_node = static_cast<const PTCreateIndex&>(tnode);
      *keyspace = cast_node.yb_table_name().namespace_name();
      *scope    = cast_node.yb_table_name().table_name();
      return &Type::CREATE_INDEX;
    }
    case TreeNodeOpcode::kPTAlterTable: {
      const auto& cast_node = static_cast<const PTAlterTable&>(tnode);
      *keyspace = cast_node.yb_table_name().namespace_name();
      *scope    = cast_node.yb_table_name().table_name();
      return &Type::ALTER_TABLE;
    }
    case TreeNodeOpcode::kPTCreateRole: {
      // Scope is not used.
      return &Type::CREATE_ROLE;
    }
    case TreeNodeOpcode::kPTAlterRole: {
      // Scope is not used.
      return &Type::ALTER_ROLE;
    }
    case TreeNodeOpcode::kPTCreateType: {
      const auto& cast_node = static_cast<const PTCreateType&>(tnode);
      *keyspace = cast_node.yb_type_name().namespace_name();
      *scope    = cast_node.yb_type_name().table_name();
      return &Type::CREATE_TYPE;
    }
    case TreeNodeOpcode::kPTGrantRevokePermission: {
      const auto& cast_node = static_cast<const PTGrantRevokePermission&>(tnode);
      // Scope hierarchy for GRANT/REVOKE is a bit unusual, see DataResource and RoleResource
      // classes in Cassandra.
      switch (cast_node.resource_type()) {
        case ALL_KEYSPACES:
          *keyspace = "data";
          *scope    = "data";
          break;
        case KEYSPACE:
          *keyspace = "data";
          *scope    = Format("data/$0", cast_node.namespace_name());
          break;
        case TABLE:
          *keyspace = Format("data/$0",    cast_node.namespace_name());
          *scope    = Format("data/$0/$1", cast_node.namespace_name(), cast_node.resource_name());
          break;
        case ALL_ROLES:
          *keyspace = "roles";
          *scope    = "roles";
          break;
        case ROLE:
          *keyspace = "roles";
          *scope    = Format("roles/$0", cast_node.resource_name());
          break;
      }
      switch (cast_node.statement_type()) {
        case GrantRevokeStatementType::GRANT:
          return &Type::GRANT;
        case GrantRevokeStatementType::REVOKE:
          return &Type::REVOKE;
      }
      FATAL_INVALID_ENUM_VALUE(GrantRevokeStatementType, cast_node.statement_type());
    }
    case TreeNodeOpcode::kPTGrantRevokeRole: {
      const auto& cast_node = static_cast<const PTGrantRevokeRole&>(tnode);
      // Scope is not used.
      switch (cast_node.statement_type()) {
        case GrantRevokeStatementType::GRANT:
          return &Type::GRANT;
        case GrantRevokeStatementType::REVOKE:
          return &Type::REVOKE;
      }
      FATAL_INVALID_ENUM_VALUE(GrantRevokeStatementType, cast_node.statement_type());
    }

    case TreeNodeOpcode::kPTListNode: {
      // As per YCQL grammar, list node can only be a DML batch.
      // Everything from PTStartTransaction to PTCommit (inclusive) is packed to PTListNode.
      return &Type::BATCH;
    }

    case TreeNodeOpcode::kPTStartTransaction: {
      // This will usually be inside PTListNode, but we can explicitly issue START TRANSACTION
      // from YCQL driver.
      return &Type::BATCH;
    }

    case TreeNodeOpcode::kPTCommit: {
      // This will usually be inside PTListNode, but we can explicitly issue COMMIT
      // from YCQL driver.
      // Also, standalone COMMIT is permitted even from ysqlsh (does nothing).
      return &Type::BATCH;
    }

    case TreeNodeOpcode::kPTExplainStmt: {
      const auto& cast_node = static_cast<const PTExplainStmt&>(tnode);
      const auto& sub_stmt  = static_cast<const PTDmlStmt&>(*cast_node.stmt());
      *keyspace = sub_stmt.table_name().namespace_name();
      *scope    = sub_stmt.table_name().table_name();
      return &Type::EXPLAIN;
    }

    case TreeNodeOpcode::kPTName:
    case TreeNodeOpcode::kPTProperty:
    case TreeNodeOpcode::kPTStatic:
    case TreeNodeOpcode::kPTConstraint:
    case TreeNodeOpcode::kPTCollection:
    case TreeNodeOpcode::kPTPrimitiveType:
    case TreeNodeOpcode::kPTColumnDefinition:
    case TreeNodeOpcode::kPTAlterColumnDefinition:
    case TreeNodeOpcode::kPTDmlUsingClauseElement:
    case TreeNodeOpcode::kPTTableRef:
    case TreeNodeOpcode::kPTOrderBy:
    case TreeNodeOpcode::kPTRoleOption:
    case TreeNodeOpcode::kPTInsertValuesClause:
    case TreeNodeOpcode::kPTInsertJsonClause:
    case TreeNodeOpcode::kPTExpr:
    case TreeNodeOpcode::kPTRef:
    case TreeNodeOpcode::kPTSubscript:
    case TreeNodeOpcode::kPTAllColumns:
    case TreeNodeOpcode::kPTAssign:
    case TreeNodeOpcode::kPTBindVar:
    case TreeNodeOpcode::kPTJsonOp:
    case TreeNodeOpcode::kPTTypeField:
    case TreeNodeOpcode::kNoOp:
      return nullptr;

  }
  FATAL_INVALID_ENUM_VALUE(TreeNodeOpcode, tnode.opcode());
}

// Replace sensitive information in a CQL command string with <REDACTED> placeholders.
// We only do this for CREATE/ALTER ROLE.
std::string ObfuscateOperation(const TreeNode& tnode, const std::string& operation) {
  if (tnode.opcode() != TreeNodeOpcode::kPTCreateRole &&
      tnode.opcode() != TreeNodeOpcode::kPTAlterRole) {
    return operation;
  }

  static const auto replacement = "<REDACTED>";
  // Using somewhat tricky code to account for escaped quotes ('') in a password.
  // We replace an entire string, including quotes.
  static const std::regex pwd_start_regex("password[\\s]*=[\\s]*'", std::regex_constants::icase);
  std::smatch m;
  if (!regex_search(operation, m, pwd_start_regex)) {
    return operation;
  }
  int pwd_start_idx = m.position() + m.length() - 1;
  int pwd_length = -1;
  for (int i = pwd_start_idx + 1; i < operation.length(); ++i) {
    if (operation[i] == '\'') {
      // If the next character is a quote too - this is an escaped quote.
      if (i < operation.length() - 1 && operation[i + 1] == '\'') {
        ++i; // Skip both quotes.
      } else {
        pwd_length = i - pwd_start_idx + 1;
        break;
      }
    }
  }
  if (pwd_length == -1) {
    return operation;
  }
  std::string copy(operation);
  copy.replace(pwd_start_idx, pwd_length, replacement);
  return copy;
}

// Follows Cassandra's view format for prettified binary log.
CHECKED_STATUS AddLogEntry(const LogEntry& e) {
  std::string str;
  str.reserve(512); // Some reasonable default that's expected to fit most of the audit records.
  str.append("AUDIT: ");
  str.append("user:");
  str.append(e.user ? *e.user : "null");
  str.append("|host:");
  str.append(AsString(e.host));
  str.append("|source:");
  str.append(AsString(e.source.address()));
  str.append("|port:");
  str.append(AsString(e.source.port()));
  str.append("|timestamp:");
  str.append(std::to_string(e.timestamp.ToInt64() / 1000));
  str.append("|type:");
  str.append(e.type->name_);
  str.append("|category:");
  str.append(AsString(e.type->category_));
  if (!e.batch_id.empty()) {
    str.append("|batch:");
    str.append(e.batch_id);
  }
  if (!e.keyspace.empty()) {
    str.append("|ks:");
    str.append(e.keyspace);
  }
  if (!e.scope.empty()) {
    str.append("|scope:");
    str.append(e.scope);
  }
  str.append("|operation:");
  str.append(e.operation);
  if (!e.error_message.empty()) {
    str.append("; ");
    str.append(e.error_message);
  }
  // Since glog uses macros, it's not too convenient to extract a log level.
  auto severity = boost::algorithm::to_lower_copy(FLAGS_ycql_audit_log_level);
  if (severity == "info") {
    LOG(INFO) << str;
    return Status::OK();
  } else if (severity == "warning") {
    LOG(WARNING) << str;
    return Status::OK();
  } else if (severity == "error") {
    LOG(ERROR) << str;
    return Status::OK();
  } else {
    return STATUS_FORMAT(InvalidArgument,
                         "$0 is not a valid severity level",
                         FLAGS_ycql_audit_log_level);
  }
}

} // anonymous namespace

//
// AuditLogger class definitions
//

AuditLogger::AuditLogger(const QLEnv& ql_env) : ql_env_(ql_env) {
}

template<class Pred>
bool AuditLogger::SatisfiesGFlag(const LogEntry& e,
                                 const std::string& gflag_name,
                                 const Pred& predicate) {
  std::string gflag_value;
  bool found = GFLAGS_NAMESPACE::GetCommandLineOption(gflag_name.c_str(), &gflag_value);
  if (!found) {
    // This should never happen as we use compile-time check in a macro.
    LOG(FATAL) << "Gflag " << gflag_name << " does not exist!";
    return false;
  }

  auto& cached = gflags_cache_[gflag_name];
  if (cached.first != gflag_value) {
    VLOG(2) << "Audit flag " << gflag_name << " = " << gflag_value
            << " cache was invalid, old value = " << cached.first;
    cached.first  = gflag_value;
    cached.second = SplitGflagList(gflag_value);
  }
  const auto& split = cached.second;

  if (!split.empty() && !predicate(split, e)) {
    VLOG(1) << "Filtered out audit record: " << e.ToString()
            << ", flag: " << gflag_name << " = " << gflag_value;
    return false;
  }
  return true;
}

// Helper macro to avoid boilerplate when using SatisfiesGFlag function.
// Returns false if the given predicate is not satisfied.
#define RETURN_IF_NOT_SATISFIES_GFLAG(gflag, entry, predicate_on_split_and_e) \
    static_assert(std::is_same<decltype(BOOST_PP_CAT(FLAGS_, gflag)), std::string&>::value, \
                  "Flag must be string"); \
    \
    if (!SatisfiesGFlag(e, BOOST_PP_STRINGIZE(gflag), \
                        [](const GflagListValue& split, const LogEntry& e) { \
                          return predicate_on_split_and_e; \
                        })) { \
      return false; \
    } \
    /**/

bool AuditLogger::ShouldBeLogged(const LogEntry& e) {
  // If a keyspace isn't present, it's not used for filtering.
  if (!e.keyspace.empty()) {
    RETURN_IF_NOT_SATISFIES_GFLAG(
        ycql_audit_included_keyspaces, e,
        Contains(split, boost::algorithm::to_upper_copy(e.keyspace)))
    RETURN_IF_NOT_SATISFIES_GFLAG(
        ycql_audit_excluded_keyspaces, e,
        !Contains(split, boost::algorithm::to_upper_copy(e.keyspace)))
  }

  RETURN_IF_NOT_SATISFIES_GFLAG(
      ycql_audit_included_categories, e,
      Contains(split, AsString(e.type->category_)))
  RETURN_IF_NOT_SATISFIES_GFLAG(
      ycql_audit_excluded_categories, e,
      !Contains(split, AsString(e.type->category_)))

  // Include filter always removes userless entries.
  RETURN_IF_NOT_SATISFIES_GFLAG(
      ycql_audit_included_users, e,
      e.user && Contains(split, boost::algorithm::to_upper_copy(*e.user)))
  RETURN_IF_NOT_SATISFIES_GFLAG(
      ycql_audit_excluded_users, e,
      !e.user || !Contains(split, boost::algorithm::to_upper_copy(*e.user)))

  return true;
}

Result<LogEntry> AuditLogger::CreateLogEntry(const Type& type,
                                             std::string keyspace,
                                             std::string scope,
                                             std::string operation,
                                             std::string error_message) {
  SCHECK(conn_ != nullptr, InternalError, "Connection is not initialized");
  auto curr_user = ql_env_.CurrentRoleName();
  if (curr_user.empty()) {
    curr_user = "anonymous";
  }
  auto entry = LogEntry {
    .user          = std::move(curr_user),
    .host          = conn_->local(),
    .source        = conn_->remote(),
    .timestamp     = DateTime::TimestampNow(),
    .type          = &type,
    .batch_id      = batch_id_,
    .keyspace      = std::move(keyspace),
    .scope         = std::move(scope),
    .operation     = std::move(operation),
    .error_message = std::move(error_message)
  };
  return std::move(entry);
}

Status AuditLogger::StartBatchRequest(int statements_count) {
  if (!FLAGS_ycql_enable_audit_log) {
    return Status::OK();
  }

  if (rescheduled_ && !batch_id_.empty()) {
    // Keep an existing batch ID in case of reschedule.
    return Status::OK();
  }

  // We cannot have sub-batches as only DMLs are allowed within a batch.
  SCHECK(batch_id_.empty(), InternalError, "Batch request mode is already active!");

  batch_id_ = AsString(boost::uuids::random_generator()());

  auto operation = Format("BatchId:[$0] - BATCH of [$1] statements", batch_id_, statements_count);

  // Cassandra uses the currently active keyspace here, for whatever reason.
  // We don't as it's cumbersome to track.
  auto entry_result = CreateLogEntry(Type::BATCH,
                                     "" /* keyspace */,
                                     "" /* scope */,
                                     std::move(operation),
                                     "" /* error_message */);
  if (PREDICT_FALSE(!entry_result.ok())) {
    batch_id_ = "";
    return entry_result.status();
  }

  if (!ShouldBeLogged(*entry_result)) {
    return Status::OK();
  }

  auto s = AddLogEntry(*entry_result);
  if (PREDICT_FALSE(!s.ok())) {
    batch_id_ = "";
  }
  return s;
}

Status AuditLogger::EndBatchRequest() {
  batch_id_ = "";
  return Status::OK();
}

Status AuditLogger::LogAuthResponse(const CQLResponse& response) {
  if (!FLAGS_ycql_enable_audit_log) {
    return Status::OK();
  }

  // Initialize entry assuming the happy case
  auto entry = VERIFY_RESULT(CreateLogEntry(Type::LOGIN_SUCCESS,
                                            "" /* keyspace */,
                                            "" /* scope */,
                                            "LOGIN SUCCESSFUL",
                                            "" /* error_message */));
  switch (response.opcode()) {
    case CQLMessage::Opcode::AUTH_SUCCESS:
      break;
    case CQLMessage::Opcode::ERROR: {
      const auto& error_respone = static_cast<const ErrorResponse&>(response);
      entry.user          = boost::none;
      entry.type          = &Type::LOGIN_ERROR;
      entry.operation     = "LOGIN FAILURE";
      entry.error_message = error_respone.message();
      break;
    }
    default:
      return STATUS_FORMAT(InternalError,
                           "$0 is not a valid opcode for an auth response",
                           static_cast<uint8_t>(response.opcode()));
  }

  if (!ShouldBeLogged(entry)) {
    return Status::OK();
  }

  return AddLogEntry(entry);
}

Status AuditLogger::LogStatement(const TreeNode* tnode,
                                 const std::string& statement,
                                 bool is_prepare) {
  // FIXME(alex): Rescheduled statements should not be logged.
  if (!FLAGS_ycql_enable_audit_log || !tnode /* || rescheduled_*/) {
    return Status::OK();
  }

  std::string keyspace;
  std::string scope;
  auto type_option = GetAuditLogTypeOption(*tnode, &keyspace, &scope);
  if (!type_option)
    return Status::OK();

  auto entry = VERIFY_RESULT(CreateLogEntry(*type_option,
                                            std::move(keyspace),
                                            std::move(scope),
                                            ObfuscateOperation(*tnode, statement),
                                            "" /* error_message */));
  if (is_prepare) {
    entry.type = &Type::PREPARE_STATEMENT;
  }

  if (!ShouldBeLogged(entry)) {
    return Status::OK();
  }

  return AddLogEntry(entry);
}

Status AuditLogger::LogStatementError(const TreeNode* tnode,
                                      const std::string& statement,
                                      const Status& error_status,
                                      bool error_is_formatted) {
  if (!FLAGS_ycql_enable_audit_log || !tnode) {
    return Status::OK();
  }

  return LogStatementError(ObfuscateOperation(*tnode, statement), error_status, error_is_formatted);
}

Status AuditLogger::LogStatementError(const std::string& statement,
                                      const Status& error_status,
                                      bool error_is_formatted) {
  if (!FLAGS_ycql_enable_audit_log) {
    return Status::OK();
  }

  SCHECK(!error_status.ok(), InvalidArgument, "Operation hasn't failed");

  const Type& type = GetErrorCode(error_status) == ErrorCode::UNAUTHORIZED
                        ? Type::UNAUTHORIZED_ATTEMPT
                        : Type::REQUEST_FAILURE;
  std::string error_message = error_status.ToUserMessage();
  if (error_is_formatted) {
    // We've already concatenated message with CQL error location.
    // Here we use dirty hack, removing three trailing lines to get rid of it.
    auto split = StringSplit(error_message, '\n');
    SCHECK(split.size() > 3, InvalidArgument, "Unexpected error message format");
    split.resize(split.size() - 3);
    error_message = boost::algorithm::join(split, "\n");
  }

  // For failed requests, we do not log keyspace and scope even if we have them.
  auto entry = VERIFY_RESULT(CreateLogEntry(type,
                                            "" /* keyspace */,
                                            "" /* scope */,
                                            statement,
                                            std::move(error_message)));

  if (!ShouldBeLogged(entry)) {
    return Status::OK();
  }

  return AddLogEntry(entry);
}

} // namespace audit
} // namespace ql
} // namespace yb
