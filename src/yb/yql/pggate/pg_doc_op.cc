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

#include "yb/yql/pggate/pg_doc_op.h"

#include <algorithm>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/algorithm/string.hpp>

#include "yb/client/table.h"
#include "yb/common/pgsql_error.h"
#include "yb/common/transaction_error.h"
#include "yb/docdb/doc_key.h"
#include "yb/util/yb_pg_errcodes.h"
#include "yb/yql/pggate/pg_txn_manager.h"
#include "yb/yql/pggate/pggate_flags.h"
#include "yb/yql/pggate/ybc_pggate.h"

using std::lower_bound;
using std::list;
using std::vector;
using std::shared_ptr;
using std::make_shared;
using std::unique_ptr;
using std::move;

using yb::client::YBPgsqlOp;
using yb::client::YBPgsqlReadOp;
using yb::client::YBPgsqlWriteOp;
using yb::client::YBOperation;

namespace yb {
namespace pggate {

PgDocResult::PgDocResult(string&& data) : data_(move(data)) {
  PgDocData::LoadCache(data_, &row_count_, &row_iterator_);
}

PgDocResult::PgDocResult(string&& data, std::list<int64_t>&& row_orders)
    : data_(move(data)), row_orders_(move(row_orders)) {
  PgDocData::LoadCache(data_, &row_count_, &row_iterator_);
}

PgDocResult::~PgDocResult() {
}

int64_t PgDocResult::NextRowOrder() {
  return row_orders_.size() > 0 ? row_orders_.front() : -1;
}

Status PgDocResult::WritePgTuple(const std::vector<PgExpr*>& targets, PgTuple *pg_tuple,
                                 int64_t *row_order) {
  int attr_num = 0;
  for (const PgExpr *target : targets) {
    if (!target->is_colref() && !target->is_aggregate()) {
      return STATUS(InternalError,
                    "Unexpected expression, only column refs or aggregates supported here");
    }
    if (target->opcode() == PgColumnRef::Opcode::PG_EXPR_COLREF) {
      attr_num = static_cast<const PgColumnRef *>(target)->attr_num();
    } else {
      attr_num++;
    }

    PgWireDataHeader header = PgDocData::ReadDataHeader(&row_iterator_);
    target->TranslateData(&row_iterator_, header, attr_num - 1, pg_tuple);
  }

  if (row_orders_.size()) {
    *row_order = row_orders_.front();
    row_orders_.pop_front();
  } else {
    *row_order = -1;
  }
  return Status::OK();
}

Status PgDocResult::ProcessSystemColumns() {
  if (syscol_processed_) {
    return Status::OK();
  }
  syscol_processed_ = true;

  for (int i = 0; i < row_count_; i++) {
    PgWireDataHeader header = PgDocData::ReadDataHeader(&row_iterator_);
    SCHECK(!header.is_null(), InternalError, "System column ybctid cannot be NULL");

    int64_t data_size;
    size_t read_size = PgDocData::ReadNumber(&row_iterator_, &data_size);
    row_iterator_.remove_prefix(read_size);

    ybctids_.emplace_back(row_iterator_.data(), data_size);
    row_iterator_.remove_prefix(data_size);
  }
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

PgDocOp::PgDocOp(const PgSession::ScopedRefPtr& pg_session,
                 const PgTableDesc::ScopedRefPtr& table_desc,
                 const PgObjectId& relation_id)
    : pg_session_(pg_session),  table_desc_(table_desc), relation_id_(relation_id) {
  exec_params_.limit_count = FLAGS_ysql_prefetch_limit;
  exec_params_.limit_offset = 0;
  exec_params_.limit_use_default = true;
}

PgDocOp::~PgDocOp() {
  // Wait for result in case request was sent.
  // Operation can be part of transaction it is necessary to complete it before transaction commit.
  if (response_.InProgress()) {
    __attribute__((unused)) auto status = response_.GetStatus(*pg_session_);
  }
}

Status PgDocOp::ExecuteInit(const PgExecParameters *exec_params) {
  end_of_data_ = false;
  if (exec_params) {
    exec_params_ = *exec_params;
  }
  return Status::OK();
}

const PgExecParameters& PgDocOp::ExecParameters() const {
  return exec_params_;
}

Result<RequestSent> PgDocOp::Execute(bool force_non_bufferable) {
  // As of 09/25/2018, DocDB doesn't cache or keep any execution state for a statement, so we
  // have to call query execution every time.
  // - Normal SQL convention: Exec, Fetch, Fetch, ...
  // - Our SQL convention: Exec & Fetch, Exec & Fetch, ...
  // This refers to the sequence of operations between this layer and the underlying tablet
  // server / DocDB layer, not to the sequence of operations between the PostgreSQL layer and this
  // layer.
  exec_status_ = SendRequest(force_non_bufferable);
  RETURN_NOT_OK(exec_status_);
  return RequestSent(response_.InProgress());
}

Status PgDocOp::GetResult(list<PgDocResult> *rowsets) {
  // If the execution has error, return without reading any rows.
  RETURN_NOT_OK(exec_status_);

  if (!end_of_data_) {
    // Send request now in case prefetching was suppressed.
    if (suppress_next_result_prefetching_ && !response_.InProgress()) {
      exec_status_ = SendRequest(true /* force_non_bufferable */);
      RETURN_NOT_OK(exec_status_);
    }

    DCHECK(response_.InProgress());
    auto rows = VERIFY_RESULT(ProcessResponse(response_.GetStatus(*pg_session_)));
    // In case ProcessResponse doesn't fail with an error
    // it should return non empty rows and/or set end_of_data_.
    DCHECK(!rows.empty() || end_of_data_);
    rowsets->splice(rowsets->end(), rows);
    // Prefetch next portion of data if needed.
    if (!(end_of_data_ || suppress_next_result_prefetching_)) {
      exec_status_ = SendRequest(true /* force_non_bufferable */);
      RETURN_NOT_OK(exec_status_);
    }
  }

  return Status::OK();
}

Result<int32_t> PgDocOp::GetRowsAffectedCount() const {
  RETURN_NOT_OK(exec_status_);
  DCHECK(end_of_data_);
  return rows_affected_count_;
}

Status PgDocOp::ClonePgsqlOps(int op_count) {
  // Allocate batch operator, one per partition.
  SCHECK(op_count > 0, InternalError, "Table must have at least one partition");
  if (pgsql_ops_.size() < op_count) {
    pgsql_ops_.resize(op_count);
    for (int idx = 0; idx < op_count; idx++) {
      pgsql_ops_[idx] = CloneFromTemplate();

      // Initialize as inactive. Turn it on when setup argument for a specific partition.
      pgsql_ops_[idx]->set_active(false);
    }

    // Set parallism_level_ to maximum possible of operators to be executed at one time.
    parallelism_level_ = pgsql_ops_.size();
  }

  return Status::OK();
}

void PgDocOp::MoveInactiveOpsOutside() {
  // Move inactive op to the end.
  const int total_op_count = pgsql_ops_.size();
  bool has_sorting_order = !batch_row_orders_.empty();
  int left_iter = 0;
  int right_iter = total_op_count - 1;
  while (true) {
    // Advance left iterator.
    while (left_iter < total_op_count && pgsql_ops_[left_iter]->is_active()) left_iter++;

    // Advance right iterator.
    while (right_iter >= 0 && !pgsql_ops_[right_iter]->is_active()) right_iter--;

    // Move inactive operator to the end by swapping the pointers.
    if (left_iter < right_iter) {
      std::swap(pgsql_ops_[left_iter], pgsql_ops_[right_iter]);
      if (has_sorting_order) {
        std::swap(batch_row_orders_[left_iter], batch_row_orders_[right_iter]);
      }
    } else {
      break;
    }
  }

  // Set active op count.
  active_op_count_ = left_iter;
}

Status PgDocOp::SendRequest(bool force_non_bufferable) {
  DCHECK(exec_status_.ok());
  DCHECK(!response_.InProgress());
  exec_status_ = SendRequestImpl(force_non_bufferable);
  return exec_status_;
}

Status PgDocOp::SendRequestImpl(bool force_non_bufferable) {
  // Populate collected information into protobuf requests before sending to DocDB.
  RETURN_NOT_OK(CreateRequests());

  // Currently, send and receive individual request of a batch is not yet supported
  // - Among statements, only queries by BASE-YBCTIDs need to be sent and received in batches
  //   to honor the order of how the BASE-YBCTIDs are kept in the database.
  // - For other type of statements, it could be more efficient to send them individually.
  SCHECK(wait_for_batch_completion_, InternalError,
         "Only send and receive the whole batch is supported");

  // Send at most "parallelism_level_" number of requests at one time.
  int32_t send_count = std::min(parallelism_level_, active_op_count_);
  response_ = VERIFY_RESULT(pg_session_->RunAsync(pgsql_ops_.data(), send_count, relation_id_,
                                                  &read_time_, force_non_bufferable));

  return Status::OK();
}

Result<std::list<PgDocResult>> PgDocOp::ProcessResponse(const Status& status) {
  // Check operation status.
  DCHECK(exec_status_.ok());
  exec_status_ = status;
  if (exec_status_.ok()) {
    auto result = ProcessResponseImpl();
    if (result.ok()) {
      return result;
    }
    exec_status_ = result.status();
  }
  return exec_status_;
}

Result<std::list<PgDocResult>> PgDocOp::ProcessResponseResult() {
  VLOG(1) << __PRETTY_FUNCTION__ << ": Received response for request " << this;

  // Check for errors reported by tablet server.
  for (int op_index = 0; op_index < active_op_count_; op_index++) {
    RETURN_NOT_OK(pg_session_->HandleResponse(*pgsql_ops_[op_index], PgObjectId()));
  }

  // Process data coming from tablet server.
  std::list<PgDocResult> result;
  bool no_sorting_order = batch_row_orders_.size() == 0;

  rows_affected_count_ = 0;
  for (int op_index = 0; op_index < active_op_count_; op_index++) {
    YBPgsqlOp *pgsql_op = pgsql_ops_[op_index].get();
    // Get total number of rows that are operated on.
    rows_affected_count_ += pgsql_op->response().rows_affected_count();

    // Get contents.
    if (!pgsql_op->rows_data().empty()) {
      if (no_sorting_order) {
        result.emplace_back(pgsql_op->rows_data());
      } else {
        result.emplace_back(pgsql_op->rows_data(), std::move(batch_row_orders_[op_index]));
      }
    }
  }

  return std::move(result);
}

void PgDocOp::SetReadTime() {
  read_time_ = exec_params_.read_time;
}

//-------------------------------------------------------------------------------------------------

PgDocReadOp::PgDocReadOp(const PgSession::ScopedRefPtr& pg_session,
                         const PgTableDesc::ScopedRefPtr& table_desc,
                         std::unique_ptr<client::YBPgsqlReadOp> read_op)
    : PgDocOp(pg_session, table_desc), template_op_(std::move(read_op)) {
}

Status PgDocReadOp::ExecuteInit(const PgExecParameters *exec_params) {
  SCHECK(pgsql_ops_.empty(),
         IllegalState,
         "Exec params can't be checked for already created operations");
  RETURN_NOT_OK(PgDocOp::ExecuteInit(exec_params));

  template_op_->mutable_request()->set_return_paging_state(true);
  SetRequestPrefetchLimit();
  SetRowMark();
  SetReadTime();
  return Status::OK();
}

Result<std::list<PgDocResult>> PgDocReadOp::ProcessResponseImpl() {
  // Process result from tablet server and check result status.
  auto result = VERIFY_RESULT(ProcessResponseResult());

  // Process paging state and check status.
  RETURN_NOT_OK(ProcessResponsePagingState());
  return std::move(result);
}

Status PgDocReadOp::CreateRequests() {
  if (request_population_completed_) {
    return Status::OK();
  }

  if (exec_params_.read_from_followers) {
    template_op_->set_yb_consistency_level(YBConsistencyLevel::CONSISTENT_PREFIX);
  }

  // All information from the SQL request has been collected and setup. This code populate
  // Protobuf requests before sending them to DocDB. For performance reasons, requests are
  // constructed differently for different statement.
  if (template_op_->request().is_aggregate()) {
    // Optimization for COUNT() operator.
    // - SELECT count(*) FROM sql_table;
    // - Multiple requests are created to run sequential COUNT() in parallel.
    return PopulateParallelSelectCountOps();

  } else if (template_op_->request().partition_column_values_size() > 0) {
    // Optimization for multiple hash keys.
    // - SELECT * FROM sql_table WHERE hash_c1 IN (1, 2, 3) AND hash_c2 IN (4, 5, 6);
    // - Multiple requests for differrent hash permutations / keys.
    return PopulateNextHashPermutationOps();

  } else {
    // No optimization.
    if (exec_params_.partition_key != nullptr) {
      RETURN_NOT_OK(SetScanPartitionBoundary());
    }
    pgsql_ops_.push_back(template_op_);
    template_op_->set_active(true);
    active_op_count_ = 1;
    request_population_completed_ = true;
    return Status::OK();
  }
}

Status PgDocReadOp::PopulateDmlByYbctidOps(const vector<Slice> *ybctids) {
  // This function is called only when ybctids were returned from INDEX.
  //
  // NOTE on a typical process.
  // 1- Statement:
  //    SELECT xxx FROM <table> WHERE ybctid IN (SELECT ybctid FROM INDEX);
  //
  // 2- Select 1024 ybctids (prefetch limit) from INDEX.
  //
  // 3- ONLY ONE TIME: Create a batch of operators, one per partition.
  //    * Each operator has a clone requests from template_op_.
  //    * We will reuse the created operators & requests for the future batches of 1024 ybctids.
  //    * Certain fields in the protobuf requests MUST BE RESET for each batches.
  //
  // 4- Assign the selected 1024 ybctids to the batch of operators.
  //
  // 5- Send requests to tablet servers to read data from <tab> associated with ybctid values.
  //
  // 6- Repeat step 2 thru 5 for the next batch of 1024 ybctids till done.
  RETURN_NOT_OK(InitializeYbctidOperators());

  // Begin a batch of ybctids.
  end_of_data_ = false;

  // Assign ybctid values.
  for (const Slice& ybctid : *ybctids) {
    // Find partition. The partition index is the boundary order minus 1.
    // - For hash partitioning, we use hashcode to find the right index.
    // - For range partitioning, we pass partition key to seek the index.
    SCHECK(ybctid.size() > 0, InternalError, "Invalid ybctid value");
    // TODO(tsplit): what if table partition is changed during PgDocReadOp lifecycle before or after
    // the following lines?
    int partition = VERIFY_RESULT(table_desc_->FindPartitionIndex(ybctid));
    SCHECK(partition >= 0 || partition < table_desc_->GetPartitionCount(), InternalError,
           "Ybctid value is not within partition boundary");

    // Assign ybctids to operators.
    YBPgsqlReadOp *read_op = GetReadOp(partition);
    if (!read_op->mutable_request()->has_ybctid_column_value()) {
      // We must set "ybctid_column_value" in the request for two reasons.
      // - "client::yb_op" uses it to set the hash_code.
      // - Rolling upgrade: Older server will read only "ybctid_column_value" as it doesn't know
      //   of ybctid-batching operation.
      read_op->set_active(true);
      read_op->mutable_request()->mutable_ybctid_column_value()->mutable_value()
        ->set_binary_value(ybctid.data(), ybctid.size());
    }

    // Append ybctid and its order to batch_arguments.
    // The "ybctid" values are returned in the same order as the row in the IndexTable. To keep
    // track of this order, each argument is assigned an order-number.
    auto batch_arg = read_op->mutable_request()->add_batch_arguments();
    batch_arg->set_order(batch_row_ordering_counter_);
    batch_arg->mutable_ybctid()->mutable_value()->set_binary_value(ybctid.data(), ybctid.size());

    // Remember the order number for each request.
    batch_row_orders_[partition].push_back(batch_row_ordering_counter_);

    // Increment counter for the next row.
    batch_row_ordering_counter_++;
  }

  // Done creating request, but not all partition or operator has arguments (inactive).
  MoveInactiveOpsOutside();
  request_population_completed_ = true;

  return Status::OK();
}

Status PgDocReadOp::InitializeYbctidOperators() {
  // TODO(tsplit): what if table partition is changed during PgDocReadOp lifecycle before or after
  // the following lines?
  int op_count = table_desc_->GetPartitionCount();

  if (batch_row_orders_.size() == 0) {
    // First batch:
    // - Create operators.
    // - Allocate row orders for each tablet server.
    // - Protobuf fields in requests are not yet set so not needed to be cleared.
    RETURN_NOT_OK(ClonePgsqlOps(op_count));
    batch_row_orders_.resize(op_count);

    // To honor the indexing order of ybctid values, for each batch of ybctid-binds, select all rows
    // in the batch and then order them before returning result to Postgres layer.
    wait_for_batch_completion_ = true;

  } else {
    // Second and later batches: Reuse all state variables.
    // - Clear row orders for this batch to be set later.
    // - Clear protobuf fields ybctids and others before reusing them in this batch.
    RETURN_NOT_OK(ResetInactivePgsqlOps());
  }
  return Status::OK();
}

Status PgDocReadOp::PopulateNextHashPermutationOps() {
  RETURN_NOT_OK(InitializeHashPermutationStates());

  // Set the index at the start of inactive operators.
  int op_count = pgsql_ops_.size();
  int op_index = active_op_count_;

  // Fill inactive operators with new hash permutations.
  const size_t hash_column_count = table_desc_->num_hash_key_columns();
  while (op_index < op_count && next_permutation_idx_ < total_permutation_count_) {
    YBPgsqlReadOp *read_op = GetReadOp(op_index++);
    read_op->set_active(true);

    int pos = next_permutation_idx_++;
    for (int c_idx = hash_column_count - 1; c_idx >= 0; --c_idx) {
      int sel_idx = pos % partition_exprs_[c_idx].size();
      read_op->mutable_request()->mutable_partition_column_values(c_idx)
          ->CopyFrom(*partition_exprs_[c_idx][sel_idx]);
      pos /= partition_exprs_[c_idx].size();
    }
  }
  active_op_count_ = op_index;

  // Stop adding requests if we reach the total number of permutations.
  request_population_completed_ = (next_permutation_idx_ >= total_permutation_count_);

  return Status::OK();
}

// Collect hash expressions to prepare for generating permutations.
Status PgDocReadOp::InitializeHashPermutationStates() {
  // Return if state variables were initialized.
  if (!partition_exprs_.empty()) {
    // Reset the protobuf request before reusing the operators.
    return ResetInactivePgsqlOps();
  }

  // Initialize partition_exprs_.
  // Reorganize the input arguments from Postgres to prepre for permutation generation.
  const size_t hash_column_count = table_desc_->num_hash_key_columns();
  partition_exprs_.resize(hash_column_count);
  for (int c_idx = 0; c_idx < hash_column_count; ++c_idx) {
    const auto& col_expr = template_op_->request().partition_column_values(c_idx);
    if (col_expr.has_condition()) {
      for (const auto& expr : col_expr.condition().operands(1).condition().operands()) {
        partition_exprs_[c_idx].push_back(&expr);
      }
    } else {
      partition_exprs_[c_idx].push_back(&col_expr);
    }
  }

  // Calculate the total number of permutations to be generated.
  total_permutation_count_ = 1;
  for (auto& exprs : partition_exprs_) {
    total_permutation_count_ *= exprs.size();
  }

  // Create operators, one operation per partition, up to FLAGS_ysql_request_limit.
  //
  // TODO(neil) The control variable "ysql_request_limit" should be applied to ALL statements, but
  // at the moment, the number of operators never exceeds the number of tablets except for hash
  // permutation operation, so the work on this GFLAG can be done when it is necessary.
  int max_op_count = std::min(total_permutation_count_, FLAGS_ysql_request_limit);
  RETURN_NOT_OK(ClonePgsqlOps(max_op_count));

  // Clear the original partition expressions as it will be replaced with hash permutations.
  for (int op_index = 0; op_index < max_op_count; op_index++) {
    YBPgsqlReadOp *read_op = GetReadOp(op_index);
    auto read_request = read_op->mutable_request();
    read_request->clear_partition_column_values();
    for (int i = 0; i < hash_column_count; ++i) {
      read_request->add_partition_column_values();
    }
    read_op->set_active(false);
  }

  // Initialize counters.
  next_permutation_idx_ = 0;
  active_op_count_ = 0;

  return Status::OK();
}

Status PgDocReadOp::PopulateParallelSelectCountOps() {
  // Create batch operators, one per partition, to SELECT COUNT() in parallel.
  // TODO(tsplit): what if table partition is changed during PgDocReadOp lifecycle before or after
  // the following line?
  RETURN_NOT_OK(ClonePgsqlOps(table_desc_->GetPartitionCount()));
  // Set "pararallelism_level_" to control how many operators can be sent at one time.
  //
  // TODO(neil) The calculation for this control variable should be applied to ALL operators, but
  // the following calculation needs to be refined before it can be used for all statements.
  parallelism_level_ = FLAGS_ysql_select_parallelism;
  if (parallelism_level_ < 0) {
    // Auto.
    int tserver_count = 0;
    RETURN_NOT_OK(pg_session_->TabletServerCount(&tserver_count, true /* primary_only */,
                                                 true /* use_cache */));

    // Establish lower and upper bounds on parallelism.
    int kMinParSelCountParallelism = 1;
    int kMaxParSelCountParallelism = 16;
    parallelism_level_ =
      std::min(std::max(tserver_count * 2, kMinParSelCountParallelism), kMaxParSelCountParallelism);
  }

  // Assign partitions to operators.
  const auto& partition_keys = table_desc_->GetPartitions();
  SCHECK_EQ(partition_keys.size(), pgsql_ops_.size(), IllegalState,
            "Number of partitions and number of partition keys are not the same");

  for (int partition = 0; partition < partition_keys.size(); partition++) {
    // Construct a new YBPgsqlReadOp.
    pgsql_ops_[partition]->set_active(true);

    // Use partition index to setup the protobuf to identify the partition that this request
    // is for. Batcher will use this information to send the request to correct tablet server, and
    // server uses this information to operate on correct tablet.
    // - Range partition uses range partition key to identify partition.
    // - Hash partition uses "next_partition_key" and "max_hash_code" to identify partition.
    string upper_bound;
    if (partition < partition_keys.size() - 1) {
      upper_bound = partition_keys[partition + 1];
    }
    RETURN_NOT_OK(table_desc_->SetScanBoundary(GetReadOp(partition)->mutable_request(),
                                               partition_keys[partition],
                                               true /* lower_bound_is_inclusive */,
                                               upper_bound,
                                               false /* upper_bound_is_inclusive */));
  }
  active_op_count_ = partition_keys.size();
  request_population_completed_ = true;

  return Status::OK();
}

// When postgres requests to scan a specific partition, set the partition parameter accordingly.
Status PgDocReadOp::SetScanPartitionBoundary() {
  // Boundary to scan from a given key to the end of its associated tablet.
  // - Lower: The given partition key (inclusive).
  // - Upper: Beginning of next tablet (not inclusive).
  SCHECK(exec_params_.partition_key != nullptr, Uninitialized, "expected non-null partition_key");

  // Seek the tablet of the given key.
  // TODO(tsplit): what if table partition is changed during PgDocReadOp lifecycle before or after
  // the following line?
  const std::vector<std::string>& partition_keys = table_desc_->GetPartitions();
  const auto& partition_key = std::find(
      partition_keys.begin(),
      partition_keys.end(),
      a2b_hex(exec_params_.partition_key));
  RSTATUS_DCHECK(
      partition_key != partition_keys.end(), InvalidArgument, "invalid partition key given");

  // Seek upper bound (Beginning of next tablet).
  string upper_bound;
  const auto& next_partition_key = std::next(partition_key, 1);
  if (next_partition_key != partition_keys.end()) {
    upper_bound = *next_partition_key;
  }
  RETURN_NOT_OK(table_desc_->SetScanBoundary(template_op_->mutable_request(),
                                             *partition_key,
                                             true /* lower_bound_is_inclusive */,
                                             upper_bound,
                                             false /* upper_bound_is_inclusive */));
  return Status::OK();
}

Status PgDocReadOp::ProcessResponsePagingState() {
  // For each read_op, set up its request for the next batch of data or make it in-active.
  bool has_more_data = false;
  int32_t send_count = std::min(parallelism_level_, active_op_count_);

  for (int op_index = 0; op_index < send_count; op_index++) {
    YBPgsqlReadOp *read_op = GetReadOp(op_index);
    RETURN_NOT_OK(ReviewResponsePagingState(read_op));

    auto& res = *read_op->mutable_response();
    // Check for completion.
    bool has_more_arg = false;
    if (res.has_paging_state()) {
      has_more_arg = true;
      PgsqlReadRequestPB *req = read_op->mutable_request();

      // Set up paging state for next request.
      // A query request can be nested, and paging state belong to the innermost query which is
      // the read operator that is operated first and feeds data to other queries.
      // Recursive Proto Message:
      //     PgsqlReadRequestPB { PgsqlReadRequestPB index_request; }
      PgsqlReadRequestPB *innermost_req = req;
      while (innermost_req->has_index_request()) {
        innermost_req = innermost_req->mutable_index_request();
      }
      *innermost_req->mutable_paging_state() = std::move(*res.mutable_paging_state());
      if (innermost_req->paging_state().has_read_time()) {
        read_op->SetReadTime(ReadHybridTime::FromPB(innermost_req->paging_state().read_time()));
      }
      // Parse/Analysis/Rewrite catalog version has already been checked on the first request.
      // The docdb layer will check the target table's schema version is compatible.
      // This allows long-running queries to continue in the presence of other DDL statements
      // as long as they do not affect the table(s) being queried.
      req->clear_ysql_catalog_version();
    }

    // Check for batch execution.
    if (res.batch_arg_count() < read_op->request().batch_arguments_size()) {
      has_more_arg = true;

      // Delete the executed arguments from batch and keep those that haven't been executed.
      PgsqlReadRequestPB *req = read_op->mutable_request();
      req->mutable_batch_arguments()->DeleteSubrange(0, res.batch_arg_count());

      // Due to rolling upgrade reason, we must copy the first batch_arg to the scalar arg.
      FormulateRequestForRollingUpgrade(read_op->mutable_request());
    }

    if (has_more_arg) {
      has_more_data = true;
    } else {
      read_op->set_active(false);
    }
  }

  if (has_more_data || send_count < active_op_count_) {
    // Move inactive ops to the end of pgsql_ops_ to make room for new set of arguments.
    MoveInactiveOpsOutside();
    end_of_data_ = false;
  } else {
    // There should be no active op left in queue.
    active_op_count_ = 0;
    end_of_data_ = request_population_completed_;
  }

  return Status::OK();
}

void PgDocReadOp::SetRequestPrefetchLimit() {
  // Predict the maximum prefetch-limit using the associated gflags.
  PgsqlReadRequestPB *req = template_op_->mutable_request();
  int predicted_limit = FLAGS_ysql_prefetch_limit;
  if (!req->is_forward_scan()) {
    // Backward scan is slower than forward scan, so predicted limit is a smaller number.
    predicted_limit = predicted_limit * FLAGS_ysql_backward_prefetch_scale_factor;
  }

  // System setting has to be at least 1 while user setting (LIMIT clause) can be anything that
  // is allowed by SQL semantics.
  if (predicted_limit < 1) {
    predicted_limit = 1;
  }

  // Use statement LIMIT(count + offset) if it is smaller than the predicted limit.
  int64_t limit_count = exec_params_.limit_count + exec_params_.limit_offset;
  suppress_next_result_prefetching_ = true;
  if (exec_params_.limit_use_default || limit_count > predicted_limit) {
    limit_count = predicted_limit;
    suppress_next_result_prefetching_ = false;
  }
  req->set_limit(limit_count);
}

void PgDocReadOp::SetRowMark() {
  PgsqlReadRequestPB *const req = template_op_->mutable_request();

  if (exec_params_.rowmark < 0) {
    req->clear_row_mark_type();
  } else {
    req->set_row_mark_type(static_cast<yb::RowMarkType>(exec_params_.rowmark));
  }
}

void PgDocReadOp::SetReadTime() {
  PgDocOp::SetReadTime();
  if (read_time_) {
    template_op_->SetReadTime(ReadHybridTime::FromUint64(read_time_));
    // TODO(jason): don't assume that read_time being set means it's always for backfill
    // (issue #6854).
    template_op_->SetIsForBackfill(true);
  }
}

Status PgDocReadOp::ResetInactivePgsqlOps() {
  // Clear the existing ybctids.
  for (int op_index = active_op_count_; op_index < pgsql_ops_.size(); op_index++) {
    PgsqlReadRequestPB *read_req = GetReadOp(op_index)->mutable_request();
    read_req->clear_ybctid_column_value();
    read_req->clear_batch_arguments();
    read_req->clear_hash_code();
    read_req->clear_max_hash_code();
    read_req->clear_paging_state();
  }

  // Clear row orders.
  if (batch_row_orders_.size() > 0) {
    for (int op_index = active_op_count_; op_index < pgsql_ops_.size(); op_index++) {
      batch_row_orders_[op_index].clear();
    }
  }

  return Status::OK();
}

void PgDocReadOp::FormulateRequestForRollingUpgrade(PgsqlReadRequestPB *read_req) {
  // Copy first batch-argument to scalar arg because older server does not support batch arguments.
  const auto& batch_arg = read_req->batch_arguments(0);

  if (batch_arg.has_ybctid()) {
    *read_req->mutable_ybctid_column_value() = std::move(batch_arg.ybctid());
  }

  if (batch_arg.partition_column_values_size() > 0) {
    read_req->set_hash_code(batch_arg.hash_code());
    read_req->set_max_hash_code(batch_arg.max_hash_code());
    *read_req->mutable_partition_column_values() =
      std::move(read_req->batch_arguments(0).partition_column_values());
  }
}

//--------------------------------------------------------------------------------------------------

PgDocWriteOp::PgDocWriteOp(const PgSession::ScopedRefPtr& pg_session,
                           const PgTableDesc::ScopedRefPtr& table_desc,
                           const PgObjectId& relation_id,
                           std::unique_ptr<YBPgsqlWriteOp> write_op)
    : PgDocOp(pg_session, table_desc, relation_id),
      write_op_(std::move(write_op)) {
}

Result<std::list<PgDocResult>> PgDocWriteOp::ProcessResponseImpl() {
  // Process result from tablet server and check result status.
  auto result = VERIFY_RESULT(ProcessResponseResult());

  // End execution and return result.
  end_of_data_ = true;
  VLOG(1) << __PRETTY_FUNCTION__ << ": Received response for request " << this;
  return std::move(result);
}

Status PgDocWriteOp::CreateRequests() {
  if (request_population_completed_) {
    return Status::OK();
  }

  // Setup a singular operator.
  pgsql_ops_.push_back(write_op_);
  write_op_->set_active(true);
  active_op_count_ = 1;
  request_population_completed_ = true;

  // Log non buffered request.
  VLOG_IF(1, response_.InProgress()) << __PRETTY_FUNCTION__ << ": Sending request for " << this;
  return Status::OK();
}

void PgDocWriteOp::SetWriteTime(const HybridTime& write_time) {
  write_op_->SetWriteTime(write_time);
}


}  // namespace pggate
}  // namespace yb
