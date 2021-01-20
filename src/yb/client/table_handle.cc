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

#include "yb/client/table_handle.h"

#include "yb/client/client.h"
#include "yb/client/error.h"
#include "yb/client/session.h"
#include "yb/client/table_creator.h"
#include "yb/client/yb_op.h"

#include "yb/master/master.pb.h"

#include "yb/yql/cql/ql/util/statement_result.h"

using namespace std::literals; // NOLINT

namespace yb {
namespace client {

Status TableHandle::Create(const YBTableName& table_name,
                           int num_tablets,
                           YBClient* client,
                           YBSchemaBuilder* builder,
                           IndexInfoPB* index_info) {
  YBSchema schema;
  RETURN_NOT_OK(builder->Build(&schema));
  return Create(table_name, num_tablets, schema, client, index_info);
}

Status TableHandle::Create(const YBTableName& table_name,
                           int num_tablets,
                           const YBSchema& schema,
                           YBClient* client,
                           IndexInfoPB* index_info) {
  std::unique_ptr<YBTableCreator> table_creator(client->NewTableCreator());
  table_creator->table_name(table_name)
      .schema(&schema)
      .num_tablets(num_tablets);

  // Setup Index properties.
  if (index_info) {
    table_creator->indexed_table_id(index_info->indexed_table_id())
        .is_local_index(index_info->is_local())
        .is_unique_index(index_info->is_unique())
        .mutable_index_info()->CopyFrom(*index_info);
  }

  RETURN_NOT_OK(table_creator->Create());
  return Open(table_name, client);
}

Status TableHandle::Open(const YBTableName& table_name, YBClient* client) {
  RETURN_NOT_OK(client->OpenTable(table_name, &table_));

  auto schema = table_->schema();
  for (size_t i = 0; i < schema.num_columns(); ++i) {
    yb::ColumnId col_id = yb::ColumnId(schema.ColumnId(i));
    column_ids_.emplace(schema.Column(i).name(), col_id);
    column_types_.emplace(col_id, schema.Column(i).type());
  }

  return Status::OK();
}

const YBTableName& TableHandle::name() const {
  return table_->name();
}

const YBSchema& TableHandle::schema() const {
  return table_->schema();
}

std::vector<std::string> TableHandle::AllColumnNames() const {
  std::vector<std::string> result;
  result.reserve(table_->schema().columns().size());
  for (const auto& column : table_->schema().columns()) {
    result.push_back(column.name());
  }
  return result;
}

namespace {

template<class T>
auto SetupRequest(const T& op, const YBSchema& schema) {
  auto* req = op->mutable_request();
  req->set_client(YQL_CLIENT_CQL);
  req->set_request_id(0);
  req->set_query_id(reinterpret_cast<int64_t>(op.get()));
  req->set_schema_version(schema.version());
  return req;
}

} // namespace

std::shared_ptr<YBqlWriteOp> TableHandle::NewWriteOp(QLWriteRequestPB::QLStmtType type) const {
  auto op = std::make_shared<YBqlWriteOp>(table_);
  auto* req = SetupRequest(op, table_->schema());
  req->set_type(type);
  return op;
}

std::shared_ptr<YBqlReadOp> TableHandle::NewReadOp() const {
  std::shared_ptr<YBqlReadOp> op(table_->NewQLRead());
  SetupRequest(op, table_->schema());
  return op;
}

QLValuePB* TableHandle::PrepareColumn(QLWriteRequestPB* req, const string& column_name) const {
  return QLPrepareColumn(req, ColumnId(column_name));
}

#define TABLE_HANDLE_TYPE_DEFINITIONS_IMPL(name, lname, type) \
void TableHandle::PP_CAT3(Add, name, ColumnValue)( \
    QLWriteRequestPB* req, const std::string &column_name, type value) const { \
  PrepareColumn(req, column_name)->PP_CAT3(set_, lname, _value)(value); \
} \
\
void TableHandle::PP_CAT3(Set, name, Condition)( \
    QLConditionPB* const condition, const string& column_name, const QLOperator op, \
    type value) const { \
  PrepareCondition(condition, column_name, op)->PP_CAT3(set_, lname, _value)(value); \
} \
\
void TableHandle::PP_CAT3(Add, name, Condition)( \
    QLConditionPB* const condition, const string& column_name, const QLOperator op, \
    type value) const { \
  PP_CAT3(Set, name, Condition)( \
    condition->add_operands()->mutable_condition(), column_name, op, value); \
} \

#define TABLE_HANDLE_TYPE_DEFINITIONS(i, data, entry) TABLE_HANDLE_TYPE_DEFINITIONS_IMPL entry

BOOST_PP_SEQ_FOR_EACH(TABLE_HANDLE_TYPE_DEFINITIONS, ~, QL_PROTOCOL_TYPES);

void TableHandle::SetColumn(QLColumnValuePB* column_value, const string& column_name) const {
  column_value->set_column_id(ColumnId(column_name));
}

QLValuePB* TableHandle::PrepareCondition(
    QLConditionPB* const condition, const string& column_name, const QLOperator op) const {
  return QLPrepareCondition(condition, ColumnId(column_name), op);
}

void TableHandle::AddCondition(QLConditionPB* const condition, const QLOperator op) const {
  condition->add_operands()->mutable_condition()->set_op(op);
}

void TableHandle::AddColumns(const std::vector<std::string>& columns, QLReadRequestPB* req) const {
  QLRSRowDescPB* rsrow_desc = req->mutable_rsrow_desc();
  for (const auto& column : columns) {
    auto id = ColumnId(column);
    req->add_selected_exprs()->set_column_id(id);
    req->mutable_column_refs()->add_ids(id);

    QLRSColDescPB* rscol_desc = rsrow_desc->add_rscol_descs();
    rscol_desc->set_name(column);
    ColumnType(column)->ToQLTypePB(rscol_desc->mutable_ql_type());
  }
}

TableIteratorOptions::TableIteratorOptions() {}

TableIterator::TableIterator() : table_(nullptr) {}

#define REPORT_AND_RETURN_IF_NOT_OK(expr) \
  do { \
    auto&& status = (expr); \
    if (!status.ok()) { HandleError(MoveStatus(status)); return; } \
  } while (false) \

#define REPORT_AND_RETURN_FALSE_IF_NOT_OK(expr) \
  do { \
    auto&& status = (expr); \
    if (!status.ok()) { HandleError(MoveStatus(status)); return false; } \
  } while (false) \

TableIterator::TableIterator(const TableHandle* table, const TableIteratorOptions& options)
    : table_(table), error_handler_(options.error_handler) {
  auto client = (*table)->client();

  session_ = client->NewSession();

  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  REPORT_AND_RETURN_IF_NOT_OK(client->GetTablets(table->name(), 0, &tablets));
  if (tablets.size() == 0) {
    table_ = nullptr;
    return;
  }
  ops_.reserve(tablets.size());
  partition_key_ends_.reserve(tablets.size());

  for (const auto& tablet : tablets) {
    if (!options.tablet.empty() && options.tablet != tablet.tablet_id()) {
      continue;
    }
    auto op = table->NewReadOp();
    auto req = op->mutable_request();
    op->set_yb_consistency_level(options.consistency);

    const auto& key_start = tablet.partition().partition_key_start();
    if (!key_start.empty()) {
      req->set_hash_code(PartitionSchema::DecodeMultiColumnHashValue(key_start));
    }

    if (options.filter) {
      options.filter(*table_, req->mutable_where_expr()->mutable_condition());
    } else {
      req->set_return_paging_state(true);
      req->set_limit(128);
    }
    if (options.read_time) {
      op->SetReadTime(options.read_time);
    }
    table_->AddColumns(*options.columns, req);
    ops_.push_back(std::move(op));
    partition_key_ends_.push_back(tablet.partition().partition_key_end());
  }

  if (ExecuteOps()) {
    Move();
  }
}

bool TableIterator::ExecuteOps() {
  constexpr size_t kMaxConcurrentOps = 5;
  const size_t new_executed_ops = std::min(ops_.size(), executed_ops_ + kMaxConcurrentOps);
  for (size_t i = executed_ops_; i != new_executed_ops; ++i) {
    REPORT_AND_RETURN_FALSE_IF_NOT_OK(session_->Apply(ops_[i]));
  }

  REPORT_AND_RETURN_FALSE_IF_NOT_OK(session_->Flush());

  for (size_t i = executed_ops_; i != new_executed_ops; ++i) {
    const auto& op = ops_[i];
    if (QLResponsePB::YQL_STATUS_OK != op->response().status()) {
      HandleError(STATUS_FORMAT(RuntimeError, "Error for $0: $1", *op, op->response()));
    }
  }

  executed_ops_ = new_executed_ops;
  return true;
}

bool TableIterator::Equals(const TableIterator& rhs) const {
  return table_ == rhs.table_;
}

TableIterator& TableIterator::operator++() {
  ++row_index_;
  Move();
  return *this;
}

const QLRow& TableIterator::operator*() const {
  return current_block_->rows()[row_index_];
}

void TableIterator::Move() {
  while (!current_block_ || row_index_ == current_block_->rows().size()) {
    if (current_block_) {
      if (paging_state_) {
        auto& op = ops_[ops_index_];
        *op->mutable_request()->mutable_paging_state() = *paging_state_;
        REPORT_AND_RETURN_IF_NOT_OK(session_->ApplyAndFlush(op));
        if (QLResponsePB::YQL_STATUS_OK != op->response().status()) {
          HandleError(STATUS_FORMAT(RuntimeError, "Error for $0: $1", *op, op->response()));
        }
      } else {
        ++ops_index_;
        if (ops_index_ >= executed_ops_ && executed_ops_ < ops_.size()) {
          if (!ExecuteOps()) {
            // Error occurred. exit out early.
            return;
          }
        }
      }
    }
    if (ops_index_ == ops_.size()) {
      table_ = nullptr;
      return;
    }
    auto& op = *ops_[ops_index_];
    auto next_block = op.MakeRowBlock();
    REPORT_AND_RETURN_IF_NOT_OK(next_block);
    current_block_ = std::move(*next_block);
    paging_state_ = op.response().has_paging_state() ? &op.response().paging_state() : nullptr;
    if (ops_index_ < partition_key_ends_.size() - 1 && paging_state_ &&
        paging_state_->next_partition_key() >= partition_key_ends_[ops_index_]) {
      paging_state_ = nullptr;
    }
    row_index_ = 0;

    VLOG(4) << "New block: " << yb::ToString(current_block_->rows())
            << ", paging: " << yb::ToString(paging_state_);
  }
}

void TableIterator::HandleError(const Status& status) {
  if (error_handler_) {
    error_handler_(status);
  } else {
    CollectedErrors errors = session_->GetAndClearPendingErrors();
    for (const auto& error : errors) {
      LOG(ERROR) << "Failed operation: " << error->failed_op().ToString()
                 << ", status: " << error->status();
    }

    LOG(FATAL) << "Failed: " << status;
  }
  // Makes this iterator == end().
  table_ = nullptr;
}

template <>
void FilterBetweenImpl<int32_t>::operator()(
    const TableHandle& table, QLConditionPB* condition) const {
  condition->set_op(QL_OP_AND);
  table.AddInt32Condition(
      condition, column_, lower_inclusive_ ? QL_OP_GREATER_THAN_EQUAL : QL_OP_GREATER_THAN,
      lower_bound_);
  table.AddInt32Condition(
      condition, column_, upper_inclusive_ ? QL_OP_LESS_THAN_EQUAL : QL_OP_LESS_THAN, upper_bound_);
}

template <>
void FilterBetweenImpl<std::string>::operator()(
    const TableHandle& table, QLConditionPB* condition) const {
  condition->set_op(QL_OP_AND);
  table.AddStringCondition(
      condition, column_, lower_inclusive_ ? QL_OP_GREATER_THAN_EQUAL : QL_OP_GREATER_THAN,
      lower_bound_);
  table.AddStringCondition(
      condition, column_, upper_inclusive_ ? QL_OP_LESS_THAN_EQUAL : QL_OP_LESS_THAN, upper_bound_);
}

void FilterGreater::operator()(const TableHandle& table, QLConditionPB* condition) const {
  table.SetInt32Condition(
      condition, column_, inclusive_ ? QL_OP_GREATER_THAN_EQUAL : QL_OP_GREATER_THAN, bound_);
}

void FilterLess::operator()(const TableHandle& table, QLConditionPB* condition) const {
  table.SetInt32Condition(
      condition, column_, inclusive_ ? QL_OP_LESS_THAN_EQUAL : QL_OP_LESS_THAN, bound_);
}

template <>
void FilterEqualImpl<std::string>::operator()(
    const TableHandle& table, QLConditionPB* condition) const {
  table.SetBinaryCondition(condition, column_, QL_OP_EQUAL, t_);
}

} // namespace client
} // namespace yb
