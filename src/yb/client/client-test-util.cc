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

#include "yb/client/client-test-util.h"

#include <vector>

#include "yb/client/client.h"
#include "yb/client/error.h"
#include "yb/client/session.h"
#include "yb/client/table_handle.h"
#include "yb/client/yb_op.h"

#include "yb/common/ql_value.h"

#include "yb/gutil/stl_util.h"
#include "yb/util/test_util.h"

namespace yb {
namespace client {

void LogSessionErrorsAndDie(const std::shared_ptr<YBSession>& session,
                            const Status& s) {
  CHECK(!s.ok());
  auto errors = session->GetAndClearPendingErrors();

  // Log only the first 10 errors.
  LOG(INFO) << errors.size() << " failed ops. First 10 errors follow";
  int i = 0;
  for (const auto& e : errors) {
    if (i == 10) {
      break;
    }
    LOG(INFO) << "Op " << e->failed_op().ToString()
              << " had status " << e->status().ToString();
    i++;
  }
  CHECK_OK(s); // will fail
}

void FlushSessionOrDie(const std::shared_ptr<YBSession>& session,
                       const std::vector<std::shared_ptr<YBqlOp>>& ops) {
  Status s = session->Flush();
  if (PREDICT_FALSE(!s.ok())) {
    LogSessionErrorsAndDie(session, s);
  }
  for (auto& op : ops) {
    CHECK_EQ(QLResponsePB::YQL_STATUS_OK, op->response().status())
        << "Status: " << QLResponsePB::QLStatus_Name(op->response().status());
  }
}

void ScanTableToStrings(const TableHandle& table, std::vector<std::string>* row_strings) {
  row_strings->clear();
  for (const auto& row : TableRange(table)) {
    row_strings->push_back(row.ToString());
  }
}

std::vector<std::string> ScanTableToStrings(const TableHandle& table) {
  std::vector<std::string> result;
  ScanTableToStrings(table, &result);
  return result;
}

int64_t CountTableRows(const TableHandle& table) {
  std::vector<std::string> rows;
  ScanTableToStrings(table, &rows);
  return rows.size();
}

std::vector<std::string> ScanToStrings(const TableRange& range) {
  std::vector<std::pair<int32_t, std::string>> rows;
  for (const auto& row : range) {
    rows.emplace_back(row.column(0).int32_value(), row.ToString());
  }
  std::sort(rows.begin(), rows.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
  std::vector<std::string> result;
  result.reserve(rows.size());
  for (auto& row : rows) {
    result.emplace_back(std::move(row.second));
  }
  return result;
}

YBSchema YBSchemaFromSchema(const Schema& schema) {
  return YBSchema(schema);
}

std::shared_ptr<YBqlReadOp> CreateReadOp(
    int32_t key, const TableHandle& table, const std::string& value_column) {
  auto op = table.NewReadOp();
  auto req = op->mutable_request();
  QLAddInt32HashValue(req, key);
  auto value_column_id = table.ColumnId(value_column);
  req->add_selected_exprs()->set_column_id(value_column_id);
  req->mutable_column_refs()->add_ids(value_column_id);

  QLRSColDescPB *rscol_desc = req->mutable_rsrow_desc()->add_rscol_descs();
  rscol_desc->set_name(value_column);
  table.ColumnType(value_column)->ToQLTypePB(rscol_desc->mutable_ql_type());
  return op;
}

}  // namespace client
}  // namespace yb
