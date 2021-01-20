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

#include "yb/client/table_alterer.h"

#include "yb/client/client.h"
#include "yb/client/client-internal.h"
#include "yb/client/schema.h"
#include "yb/client/schema-internal.h"

namespace yb {
namespace client {

YBTableAlterer::YBTableAlterer(YBClient* client, const YBTableName& name)
  : client_(client), table_name_(name) {
}

YBTableAlterer::YBTableAlterer(YBClient* client, const string id)
  : client_(client), table_id_(std::move(id)) {
}

YBTableAlterer::~YBTableAlterer() {
}

YBTableAlterer* YBTableAlterer::RenameTo(const YBTableName& new_name) {
  rename_to_ = new_name;
  return this;
}

YBColumnSpec* YBTableAlterer::AddColumn(const string& name) {
  steps_.push_back({master::AlterTableRequestPB::ADD_COLUMN, std::make_unique<YBColumnSpec>(name)});
  return steps_.back().spec.get();
}

YBColumnSpec* YBTableAlterer::AlterColumn(const string& name) {
  steps_.push_back(
      {master::AlterTableRequestPB::ALTER_COLUMN, std::make_unique<YBColumnSpec>(name)});
  return steps_.back().spec.get();
}

YBTableAlterer* YBTableAlterer::DropColumn(const string& name) {
  steps_.push_back(
      {master::AlterTableRequestPB::DROP_COLUMN, std::make_unique<YBColumnSpec>(name)});
  return this;
}

YBTableAlterer* YBTableAlterer::SetTableProperties(const TableProperties& table_properties) {
  table_properties_ = table_properties;
  return this;
}

YBTableAlterer* YBTableAlterer::replication_info(const master::ReplicationInfoPB& ri) {
  replication_info_.emplace(ri);
  return this;
}

YBTableAlterer* YBTableAlterer::SetWalRetentionSecs(const uint32_t wal_retention_secs) {
  wal_retention_secs_ = wal_retention_secs;
  return this;
}

YBTableAlterer* YBTableAlterer::timeout(const MonoDelta& timeout) {
  timeout_ = timeout;
  return this;
}

YBTableAlterer* YBTableAlterer::wait(bool wait) {
  wait_ = wait;
  return this;
}

Status YBTableAlterer::Alter() {
  master::AlterTableRequestPB req;
  RETURN_NOT_OK(ToRequest(&req));

  MonoDelta timeout = timeout_.Initialized() ?
    timeout_ :
    client_->default_admin_operation_timeout();
  auto deadline = CoarseMonoClock::Now() + timeout;
  RETURN_NOT_OK(client_->data_->AlterTable(client_, req, deadline));
  if (wait_) {
    YBTableName alter_name = rename_to_.get_value_or(table_name_);
    RETURN_NOT_OK(client_->data_->WaitForAlterTableToFinish(
        client_, alter_name, table_id_, deadline));
  }

  return Status::OK();
}

Status YBTableAlterer::ToRequest(master::AlterTableRequestPB* req) {
  if (!status_.ok()) {
    return status_;
  }

  if (!rename_to_ && steps_.empty() && !table_properties_ && !wal_retention_secs_ &&
      !replication_info_) {
    return STATUS(InvalidArgument, "No alter steps provided");
  }

  req->Clear();

  if (table_name_.has_table()) {
    table_name_.SetIntoTableIdentifierPB(req->mutable_table());
  }

  if (!table_id_.empty()) {
    (req->mutable_table())->set_table_id(table_id_);
  }

  if (rename_to_.is_initialized()) {
    req->set_new_table_name(rename_to_.get().table_name());

    if (rename_to_.get().has_namespace()) {
      req->mutable_new_namespace()->set_name(rename_to_.get().namespace_name());
    }
  }

  for (const Step& s : steps_) {
    auto* pb_step = req->add_alter_schema_steps();
    pb_step->set_type(s.step_type);

    switch (s.step_type) {
      case master::AlterTableRequestPB::ADD_COLUMN:
      {
        YBColumnSchema col;
        RETURN_NOT_OK(s.spec->ToColumnSchema(&col));
        ColumnSchemaToPB(*col.col_,
                         pb_step->mutable_add_column()->mutable_schema());
        break;
      }
      case master::AlterTableRequestPB::DROP_COLUMN:
      {
        pb_step->mutable_drop_column()->set_name(s.spec->data_->name);
        break;
      }
      case master::AlterTableRequestPB::ALTER_COLUMN:
        // TODO(KUDU-861): support altering a column in the wire protocol.
        // For now, we just give an error if the caller tries to do
        // any operation other than rename.
        if (s.spec->data_->has_type ||
            s.spec->data_->has_nullable ||
            s.spec->data_->primary_key) {
          return STATUS(NotSupported, "cannot support AlterColumn of this type",
                                      s.spec->data_->name);
        }
        // We only support rename column
        if (!s.spec->data_->has_rename_to) {
          return STATUS(InvalidArgument, "no alter operation specified",
                                         s.spec->data_->name);
        }
        pb_step->mutable_rename_column()->set_old_name(s.spec->data_->name);
        pb_step->mutable_rename_column()->set_new_name(s.spec->data_->rename_to);
        pb_step->set_type(master::AlterTableRequestPB::RENAME_COLUMN);
        break;
      default:
        LOG(FATAL) << "unknown step type " << s.step_type;
    }
  }

  if (table_properties_.is_initialized()) {
    table_properties_->ToTablePropertiesPB(req->mutable_alter_properties());
  }

  if (wal_retention_secs_) {
    req->set_wal_retention_secs(*wal_retention_secs_);
  }

  if (replication_info_) {
    // TODO: Maybe add checks for the sanity of the replication_info.
    req->mutable_replication_info()->CopyFrom(replication_info_.get());
  }

  return Status::OK();
}

} // namespace client
} // namespace yb
