// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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
#ifndef YB_CLIENT_TABLE_ALTERER_INTERNAL_H
#define YB_CLIENT_TABLE_ALTERER_INTERNAL_H

#include <string>
#include <vector>
#include <boost/optional.hpp>

#include "yb/client/client.h"
#include "yb/master/master.pb.h"
#include "yb/util/status.h"

namespace yb {
namespace master {
class AlterTableRequestPB_AlterColumn;
} // namespace master
namespace client {

class YBColumnSpec;

class YBTableAlterer::Data {
 public:
  Data(YBClient* client, YBTableName name);
  Data(YBClient* client, string id);

    ~Data();
  CHECKED_STATUS ToRequest(master::AlterTableRequestPB* req);


  YBClient* const client_;
  const YBTableName table_name_;
  const string table_id_;

  Status status_;

  struct Step {
    master::AlterTableRequestPB::StepType step_type;

    // Owned by YBTableAlterer::Data.
    YBColumnSpec *spec;
  };
  std::vector<Step> steps_;

  MonoDelta timeout_;

  bool wait_;

  boost::optional<YBTableName> rename_to_;

  boost::optional<TableProperties> table_properties_;

 private:
  DISALLOW_COPY_AND_ASSIGN(Data);
};

} // namespace client
} // namespace yb

#endif // YB_CLIENT_TABLE_ALTERER_INTERNAL_H
