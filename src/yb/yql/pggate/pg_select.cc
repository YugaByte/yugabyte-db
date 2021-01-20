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

#include "yb/yql/pggate/pg_select.h"
#include "yb/yql/pggate/util/pg_doc_data.h"
#include "yb/client/yb_op.h"
#include "yb/docdb/primitive_value.h"

namespace yb {
namespace pggate {

using std::make_shared;

//--------------------------------------------------------------------------------------------------
// PgSelect
//--------------------------------------------------------------------------------------------------

PgSelect::PgSelect(PgSession::ScopedRefPtr pg_session, const PgObjectId& table_id,
                   const PgObjectId& index_id, const PgPrepareParameters *prepare_params)
    : PgDmlRead(pg_session, table_id, index_id, prepare_params) {}

PgSelect::~PgSelect() {
}

Status PgSelect::Prepare() {
  // Prepare target and bind descriptors.
  if (!prepare_params_.use_secondary_index) {
    target_desc_ = bind_desc_ = VERIFY_RESULT(pg_session_->LoadTable(table_id_));
  } else {
    target_desc_ = VERIFY_RESULT(pg_session_->LoadTable(table_id_));
    bind_desc_ = nullptr;

    // Create secondary index query.
    secondary_index_query_ =
      std::make_unique<PgSelectIndex>(pg_session_, table_id_, index_id_, &prepare_params_);
  }

  // Allocate READ requests to send to DocDB.
  auto read_op = target_desc_->NewPgsqlSelect();
  read_req_ = read_op->mutable_request();
  auto doc_op = make_shared<PgDocReadOp>(pg_session_, target_desc_, std::move(read_op));

  // Prepare the index selection if this operation is using the index.
  RETURN_NOT_OK(PrepareSecondaryIndex());

  // Prepare binds for the request.
  PrepareBinds();

  // Preparation complete.
  doc_op_ = doc_op;
  return Status::OK();
}

Status PgSelect::PrepareSecondaryIndex() {
  if (!secondary_index_query_) {
    // This DML statement is not using secondary index.
    return Status::OK();
  }

  // Prepare the index operation to read ybctids from the index table. There are two different
  // scenarios on how ybctids are requested.
  // - Due to an optimization in DocDB, for colocated tables (both system and user colocated), index
  //   request is sent as a part of the actual read request using protobuf field
  //   "PgsqlReadRequestPB::index_request"
  //
  //   For this case, "mutable_index_request" is allocated here and passed to PgSelectIndex node to
  //   fill in with bind-values when necessary.
  //
  // - For regular tables, the index subquery will send separate request to tablet servers collect
  //   batches of ybctids which is then used by 'this' outer select to query actual data.
  PgsqlReadRequestPB *index_req = nullptr;
  if (prepare_params_.querying_colocated_table) {
    // Allocate "index_request" and pass to PgSelectIndex.
    index_req = read_req_->mutable_index_request();
  }

  // Prepare subquery. When index_req is not null, it is part of 'this' SELECT request. When it
  // is nullptr, the subquery will create its own doc_op to run a separate read request.
  return secondary_index_query_->PrepareSubquery(index_req);
}

}  // namespace pggate
}  // namespace yb
