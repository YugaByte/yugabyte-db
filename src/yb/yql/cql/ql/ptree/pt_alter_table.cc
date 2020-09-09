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
// Treenode definitions for ALTER TABLE statements.
//--------------------------------------------------------------------------------------------------

#include "yb/yql/cql/ql/ptree/pt_alter_table.h"
#include "yb/yql/cql/ql/ptree/sem_context.h"
#include "yb/client/table.h"


namespace yb {
namespace ql {

//--------------------------------------------------------------------------------------------------

PTAlterTable::PTAlterTable(MemoryContext *memctx,
                           YBLocation::SharedPtr loc,
                           PTQualifiedName::SharedPtr name,
                           const PTListNode::SharedPtr &commands)
  : TreeNode(memctx, loc),
    name_(name),
    commands_(commands),
    table_columns_(memctx),
    mod_columns_(memctx),
    mod_props_(memctx) {
}

PTAlterTable::~PTAlterTable() {
}

CHECKED_STATUS PTAlterTable::Analyze(SemContext *sem_context) {
  // Populate internal table_ variable.
  bool is_system_ignored = false;
  RETURN_NOT_OK(name_->AnalyzeName(sem_context, OBJECT_TABLE));

  // Permissions check happen in LookupTable if flag use_cassandra_authentication is enabled.
  RETURN_NOT_OK(sem_context->LookupTable(name_->ToTableName(), name_->loc(), true /* write_table */,
                                         PermissionType::ALTER_PERMISSION,
                                         &table_, &is_system_ignored, &table_columns_));

  // Save context state, and set "this" as current table being altered.
  SymbolEntry cached_entry = *sem_context->current_processing_id();
  sem_context->set_current_alter_table(this);

  // Process alter commands.
  RETURN_NOT_OK(commands_->Analyze(sem_context));

  // Restore saved context state.
  sem_context->set_current_processing_id(cached_entry);

  if (VLOG_IS_ON(3)) {
    PrintSemanticAnalysisResult(sem_context);
  }
  return Status::OK();
}

void PTAlterTable::PrintSemanticAnalysisResult(SemContext *sem_context) {
  MCString sem_output("\tAltering Table ", sem_context->PTempMem());
  sem_output += yb_table_name().ToString().c_str();
  sem_output += "(";
  // TODO: Add debugging info for what this alter command is.
  sem_output += ")";
  VLOG(3) << "SEMANTIC ANALYSIS RESULT (" << *loc_ << "):\n" << sem_output;
}

CHECKED_STATUS PTAlterTable::AppendModColumn(SemContext *sem_context,
                                             PTAlterColumnDefinition *column) {
  // Make sure column already exists and isn't key column.
  if (column->old_name() != nullptr) {
    const ColumnDesc* desc = sem_context->GetColumnDesc(column->old_name()->last_name());
    if (desc == nullptr) {
      return sem_context->Error(this, "Column doesn't exist", ErrorCode::UNDEFINED_COLUMN);
    }

    if (desc->is_hash() && column->mod_type() != ALTER_RENAME) {
      return sem_context->Error(this, "Can't alter key column", ErrorCode::ALTER_KEY_COLUMN);
    }

    if (column->mod_type() == ALTER_DROP) {
      // Check column dependencies.
      const ColumnId column_id(desc->id());
      for (const auto& index_item : table_->index_map()) {
        const auto& index = index_item.second;
        // Check if this "index" is dependent on the column being dropped.
        if (index.CheckColumnDependency(column_id)) {
          auto index_table = sem_context->GetTableDesc(index.table_id());
          return sem_context->Error(this,
              Format("Can't drop indexed column. Remove '$0' index first and try again",
                  (index_table ? index_table->name().table_name() : "-unknown-")),
              ErrorCode::FEATURE_NOT_YET_IMPLEMENTED);
        }
      }
    }
  }

  // Make sure column already doesn't exist with the same name.
  if (column->new_name() != nullptr) {
    MCString name = *column->new_name();
    const ColumnDesc* desc = sem_context->GetColumnDesc(name);
    if (desc != nullptr) {
      // Expecting the error message matching to the reg-exp: "[Ii]nvalid column name"
      // for the error correct handling in tools (like Kong).
      return sem_context->Error(this,
          Format("Invalid column name because it conflicts with the existing column: $0",
              name.c_str()),
          ErrorCode::DUPLICATE_COLUMN);
    }
  }

  mod_columns_.push_back(column);
  return Status::OK();
}

CHECKED_STATUS PTAlterTable::AppendAlterProperty(SemContext *sem_context, PTTableProperty *prop) {
  mod_props_.push_back(prop);
  return Status::OK();
}

CHECKED_STATUS PTAlterTable::ToTableProperties(TableProperties *table_properties) const {
  for (const auto& table_property : mod_props_) {
      RETURN_NOT_OK(table_property->SetTableProperty(table_properties));
  }

  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

PTAlterColumnDefinition::PTAlterColumnDefinition(MemoryContext *memctx,
                         YBLocation::SharedPtr loc,
                         PTQualifiedName::SharedPtr name,
                         const MCSharedPtr<MCString>& new_name,
                         const PTBaseType::SharedPtr& datatype,
                         AlterColumnType type)
  : TreeNode(memctx, loc),
    name_(name),
    new_name_(new_name),
    datatype_(datatype),
    type_(type) {
}

PTAlterColumnDefinition::~PTAlterColumnDefinition() {
}

CHECKED_STATUS PTAlterColumnDefinition::Analyze(SemContext *sem_context) {
  if (name_ != nullptr) {
    RETURN_NOT_OK(name_->Analyze(sem_context));
  }

  if (new_name_ != nullptr) {
    RETURN_NOT_OK(sem_context->MapSymbol(*new_name_, this));
  }

  if (datatype_ != nullptr) {
    RETURN_NOT_OK(datatype_->Analyze(sem_context));
  }

  PTAlterTable *table = sem_context->current_alter_table();
  RETURN_NOT_OK(table->AppendModColumn(sem_context, this));

  return Status::OK();
}

}  // namespace ql
}  // namespace yb
