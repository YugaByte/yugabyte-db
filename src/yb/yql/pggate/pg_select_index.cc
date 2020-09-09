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

#include "yb/yql/pggate/pg_select_index.h"
#include "yb/yql/pggate/util/pg_doc_data.h"
#include "yb/client/yb_op.h"
#include "yb/docdb/primitive_value.h"

namespace yb {
namespace pggate {

using std::make_shared;
using std::list;

//--------------------------------------------------------------------------------------------------
// PgSelectIndex
//--------------------------------------------------------------------------------------------------

PgSelectIndex::PgSelectIndex(PgSession::ScopedRefPtr pg_session,
                             const PgObjectId& table_id,
                             const PgObjectId& index_id,
                             const PgPrepareParameters *prepare_params)
    : PgDmlRead(pg_session, table_id, index_id, prepare_params) {
}

PgSelectIndex::~PgSelectIndex() {
}

Status PgSelectIndex::Prepare() {
  // We get here only if this is an IndexOnly scan.
  CHECK(prepare_params_.index_only_scan) << "Unexpected IndexOnly scan type";
  return PrepareQuery(nullptr);
}

Status PgSelectIndex::PrepareSubquery(PgsqlReadRequestPB *read_req) {
  // We get here if this is an SecondaryIndex scan.
  CHECK(prepare_params_.use_secondary_index && !prepare_params_.index_only_scan)
    << "Unexpected Index scan type";
  return PrepareQuery(read_req);
}

Status PgSelectIndex::PrepareQuery(PgsqlReadRequestPB *read_req) {
  // Setup target and bind descriptor.
  target_desc_ = bind_desc_ = VERIFY_RESULT(pg_session_->LoadTable(index_id_));

  // Allocate READ requests to send to DocDB.
  if (read_req) {
    // For (system and user) colocated tables, SelectIndex is a part of Select and being sent
    // together with the SELECT protobuf request. A read doc_op and request is not needed in this
    // case.
    DSCHECK(prepare_params_.querying_colocated_table, InvalidArgument, "Read request invalid");
    read_req_ = read_req;
    read_req_->set_table_id(index_id_.GetYBTableId());
    doc_op_ = nullptr;

  } else {
    auto read_op = target_desc_->NewPgsqlSelect();
    read_req_ = read_op->mutable_request();
    doc_op_ = make_shared<PgDocReadOp>(pg_session_, target_desc_, std::move(read_op));
  }

  // Prepare index key columns.
  PrepareBinds();
  return Status::OK();
}

Result<bool> PgSelectIndex::FetchYbctidBatch(const vector<Slice> **ybctids) {
  // Keep reading until we get one batch of ybctids or EOF.
  while (!VERIFY_RESULT(GetNextYbctidBatch())) {
    if (!VERIFY_RESULT(FetchDataFromServer())) {
      // Server returns no more rows.
      *ybctids = nullptr;
      return false;
    }
  }

  // Got the next batch of ybctids.
  DCHECK(!rowsets_.empty());
  *ybctids = &rowsets_.front().ybctids();
  return true;
}

Result<bool> PgSelectIndex::GetNextYbctidBatch() {
  for (auto rowset_iter = rowsets_.begin(); rowset_iter != rowsets_.end();) {
    if (rowset_iter->is_eof()) {
      rowset_iter = rowsets_.erase(rowset_iter);
    } else {
      // Write all found rows to ybctid array.
      RETURN_NOT_OK(rowset_iter->ProcessSystemColumns());
      return true;
    }
  }

  return false;
}

}  // namespace pggate
}  // namespace yb
