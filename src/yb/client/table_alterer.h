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

#ifndef YB_CLIENT_TABLE_ALTERER_H
#define YB_CLIENT_TABLE_ALTERER_H

#include <boost/optional.hpp>

#include "yb/client/client_fwd.h"
#include "yb/client/yb_table_name.h"

#include "yb/common/common_fwd.h"
#include "yb/common/schema.h"

#include "yb/util/monotime.h"

#include "yb/master/master.pb.h"

namespace yb {
namespace client {

// Alters an existing table based on the provided steps.
//
// Sample usage:
//   YBTableAlterer* alterer = client->NewTableAlterer("table-name");
//   alterer->AddColumn("foo")->Type(INT32)->NotNull();
//   Status s = alterer->Alter();
//   delete alterer;
class YBTableAlterer {
 public:
  ~YBTableAlterer();

  // Renames the table.
  // If there is no new namespace (only the new table name provided), that means that the table
  // namespace must not be changed (changing the table name only in the same namespace).
  YBTableAlterer* RenameTo(const YBTableName& new_name);

  // Adds a new column to the table.
  //
  // When adding a column, you must specify the default value of the new
  // column using YBColumnSpec::DefaultValue(...).
  YBColumnSpec* AddColumn(const std::string& name);

  // Alter an existing column.
  YBColumnSpec* AlterColumn(const std::string& name);

  // Drops an existing column from the table.
  YBTableAlterer* DropColumn(const std::string& name);

  // Alter table properties.
  YBTableAlterer* SetTableProperties(const TableProperties& table_properties);

  YBTableAlterer* SetWalRetentionSecs(const uint32_t wal_retention_secs);

  // Set the timeout for the operation. This includes any waiting
  // after the alter has been submitted (i.e if the alter is slow
  // to be performed on a large table, it may time out and then
  // later be successful).
  YBTableAlterer* timeout(const MonoDelta& timeout);

  // Wait for the table to be fully altered before returning.
  //
  // If not provided, defaults to true.
  YBTableAlterer* wait(bool wait);

  // Set replication info for the table.
  YBTableAlterer* replication_info(const master::ReplicationInfoPB& ri);

  // Alters the table.
  //
  // The return value may indicate an error in the alter operation, or a
  // misuse of the builder (e.g. add_column() with default_value=NULL); in
  // the latter case, only the last error is returned.
  CHECKED_STATUS Alter();

 private:
  friend class YBClient;

  YBTableAlterer(YBClient* client, const YBTableName& name);
  YBTableAlterer(YBClient* client, const string id);

  CHECKED_STATUS ToRequest(master::AlterTableRequestPB* req);

  YBClient* const client_;
  const YBTableName table_name_;
  const string table_id_;

  Status status_;

  struct Step {
    master::AlterTableRequestPB::StepType step_type;

    std::unique_ptr<YBColumnSpec> spec;
  };
  std::vector<Step> steps_;

  MonoDelta timeout_;

  bool wait_ = true;

  boost::optional<YBTableName> rename_to_;

  boost::optional<TableProperties> table_properties_;

  boost::optional<uint32_t> wal_retention_secs_;

  boost::optional<master::ReplicationInfoPB> replication_info_;

  DISALLOW_COPY_AND_ASSIGN(YBTableAlterer);
};

} // namespace client
} // namespace yb

#endif // YB_CLIENT_TABLE_ALTERER_H
