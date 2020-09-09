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
// Treenode definitions for TRUNCATE statements.
//--------------------------------------------------------------------------------------------------

#include "yb/yql/cql/ql/ptree/pt_truncate.h"

#include "yb/client/table.h"

#include "yb/yql/cql/ql/ptree/sem_context.h"

DECLARE_bool(use_cassandra_authentication);
DECLARE_bool(ycql_require_drop_privs_for_truncate);

namespace yb {
namespace ql {

PTTruncateStmt::PTTruncateStmt(MemoryContext *memctx,
                               YBLocation::SharedPtr loc,
                               PTQualifiedNameListNode::SharedPtr names)
    : TreeNode(memctx, loc), names_(names) {
}

PTTruncateStmt::~PTTruncateStmt() {
}

Status PTTruncateStmt::Analyze(SemContext *sem_context) {
  if (names_->size() > 1) {
    return sem_context->Error(names_, "Only one table name is allowed in a truncate statement",
                              ErrorCode::CQL_STATEMENT_INVALID);
  }

  // Processing table name.
  bool is_system_ignored = false;
  RETURN_NOT_OK(name()->AnalyzeName(sem_context, OBJECT_TABLE));

  // Permissions check happen in LookupTable if flag use_cassandra_authentication is enabled.
  if (FLAGS_ycql_require_drop_privs_for_truncate) {
    return sem_context->LookupTable(yb_table_name(), name()->loc(), true /* write_table */,
                                    PermissionType::DROP_PERMISSION,
                                    &table_, &is_system_ignored);
  }
  return sem_context->LookupTable(yb_table_name(), name()->loc(), true /* write_table */,
                                  PermissionType::MODIFY_PERMISSION,
                                  &table_, &is_system_ignored);
}

void PTTruncateStmt::PrintSemanticAnalysisResult(SemContext *sem_context) {
  VLOG(3) << "SEMANTIC ANALYSIS RESULT (" << *loc_ << "):\n TRUNCATE "
          << yb_table_name().ToString();
}

const std::string& PTTruncateStmt::table_id() const {
  return table_->id();
}

}  // namespace ql
}  // namespace yb
