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

#include "yb/docdb/cql_operation.h"

#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "yb/common/index.h"
#include "yb/common/jsonb.h"
#include "yb/common/partition.h"
#include "yb/common/ql_protocol_util.h"
#include "yb/common/ql_resultset.h"
#include "yb/common/ql_storage_interface.h"
#include "yb/common/ql_value.h"

#include "yb/docdb/doc_ql_scanspec.h"
#include "yb/docdb/docdb_debug.h"
#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/docdb/docdb_util.h"
#include "yb/docdb/primitive_value_util.h"

#include "yb/util/bfpg/tserver_opcodes.h"
#include "yb/util/flag_tags.h"
#include "yb/util/trace.h"

#include "yb/yql/cql/ql/util/errcodes.h"

DEFINE_test_flag(bool, pause_write_apply_after_if, false,
                 "Pause application of QLWriteOperation after evaluating if condition.");

DEFINE_bool(ycql_consistent_transactional_paging, false,
            "Whether to enforce consistency of data returned for second page and beyond for YCQL "
            "queries on transactional tables. If true, read restart errors could be returned to "
            "prevent inconsistency. If false, no read restart errors are returned but the data may "
            "be stale. The latter is preferable for long scans. The data returned for the first "
            "page of results is never stale regardless of this flag.");

DEFINE_bool(ycql_disable_index_updating_optimization, false,
            "If true all secondary indexes must be updated even if the update does not change "
            "the index data.");
TAG_FLAG(ycql_disable_index_updating_optimization, advanced);

DECLARE_bool(trace_docdb_calls);

namespace yb {
namespace docdb {

using std::pair;
using std::unordered_map;
using std::unordered_set;
using std::vector;

namespace {

// Append dummy entries in schema to table_row
// TODO(omer): this should most probably be added somewhere else
void AddProjection(const Schema& schema, QLTableRow* table_row) {
  for (size_t i = 0; i < schema.num_columns(); i++) {
    const auto& column_id = schema.column_id(i);
    table_row->AllocColumn(column_id);
  }
}

// Create projection schemas of static and non-static columns from a rowblock projection schema
// (for read) and a WHERE / IF condition (for read / write). "schema" is the full table schema
// and "rowblock_schema" is the selected columns from which we are splitting into static and
// non-static column portions.
CHECKED_STATUS CreateProjections(const Schema& schema, const QLReferencedColumnsPB& column_refs,
                                 Schema* static_projection, Schema* non_static_projection) {
  // The projection schemas are used to scan docdb.
  unordered_set<ColumnId> static_columns, non_static_columns;

  // Add regular columns.
  for (int32_t id : column_refs.ids()) {
    const ColumnId column_id(id);
    if (!schema.is_key_column(column_id)) {
      non_static_columns.insert(column_id);
    }
  }

  // Add static columns.
  for (int32_t id : column_refs.static_ids()) {
    const ColumnId column_id(id);
    static_columns.insert(column_id);
  }

  RETURN_NOT_OK(
      schema.CreateProjectionByIdsIgnoreMissing(
          vector<ColumnId>(static_columns.begin(), static_columns.end()),
          static_projection));
  RETURN_NOT_OK(
      schema.CreateProjectionByIdsIgnoreMissing(
          vector<ColumnId>(non_static_columns.begin(), non_static_columns.end()),
          non_static_projection));

  return Status::OK();
}

CHECKED_STATUS PopulateRow(const QLTableRow& table_row, const Schema& schema,
                           const size_t begin_idx, const size_t col_count,
                           QLRow* row, size_t *col_idx) {
  for (size_t i = begin_idx; i < begin_idx + col_count; i++) {
    RETURN_NOT_OK(table_row.GetValue(schema.column_id(i), row->mutable_column((*col_idx)++)));
  }
  return Status::OK();
}

CHECKED_STATUS PopulateRow(const QLTableRow& table_row, const Schema& projection,
                           QLRow* row, size_t* col_idx) {
  return PopulateRow(table_row, projection, 0, projection.num_columns(), row, col_idx);
}

// Outer join a static row with a non-static row.
// A join is successful if and only if for every hash key, the values in the static and the
// non-static row are either non-NULL and the same, or one of them is NULL. Therefore we say that
// a join is successful if the static row is empty, and in turn return true.
// Copies the entries from the static row into the non-static one.
bool JoinStaticRow(
    const Schema& schema, const Schema& static_projection, const QLTableRow& static_row,
    QLTableRow* non_static_row) {
  // The join is successful if the static row is empty
  if (static_row.IsEmpty()) {
    return true;
  }

  // Now we know that the static row is not empty. The non-static row cannot be empty, therefore
  // we know that both the static row and the non-static one have non-NULL entries for all
  // hash keys. Therefore if MatchColumn returns false, we know the join is unsuccessful.
  // TODO(neil)
  // - Need to assign TTL and WriteTime to their default values.
  // - Check if they should be compared and copied over. Most likely not needed as we don't allow
  //   selecting TTL and WriteTime for static columns.
  // - This copying function should be moved to QLTableRow class.
  for (size_t i = 0; i < schema.num_hash_key_columns(); i++) {
    if (!non_static_row->MatchColumn(schema.column_id(i), static_row)) {
      return false;
    }
  }

  // Join the static columns in the static row into the non-static row.
  for (size_t i = 0; i < static_projection.num_columns(); i++) {
    non_static_row->CopyColumn(static_projection.column_id(i), static_row);
  }

  return true;
}

// Join a non-static row with a static row.
// Returns true if the two rows match
bool JoinNonStaticRow(
    const Schema& schema, const Schema& static_projection, const QLTableRow& non_static_row,
    QLTableRow* static_row) {
  bool join_successful = true;

  for (size_t i = 0; i < schema.num_hash_key_columns(); i++) {
    if (!static_row->MatchColumn(schema.column_id(i), non_static_row)) {
      join_successful = false;
      break;
    }
  }

  if (!join_successful) {
    static_row->Clear();
    for (size_t i = 0; i < static_projection.num_columns(); i++) {
      static_row->AllocColumn(static_projection.column_id(i));
    }

    for (size_t i = 0; i < schema.num_hash_key_columns(); i++) {
      static_row->CopyColumn(schema.column_id(i), non_static_row);
    }
  }
  return join_successful;
}

CHECKED_STATUS FindMemberForIndex(const QLColumnValuePB& column_value,
                                  size_t index,
                                  rapidjson::Value* document,
                                  rapidjson::Value::MemberIterator* memberit,
                                  rapidjson::Value::ValueIterator* valueit,
                                  bool* last_elem_object,
                                  bool is_insert) {
  *last_elem_object = false;

  int64_t array_index;
  if (document->IsArray()) {
    util::VarInt varint;
    RETURN_NOT_OK(varint.DecodeFromComparable(
        column_value.json_args(index).operand().value().varint_value()));
    array_index = VERIFY_RESULT(varint.ToInt64());

    if (array_index >= document->GetArray().Size() || array_index < 0) {
      return STATUS_SUBSTITUTE(QLError, "Array index out of bounds: ", array_index);
    }
    *valueit = document->Begin();
    std::advance(*valueit, array_index);
  } else if (document->IsObject()) {
    if (!is_insert) {
      util::VarInt varint;
      auto status =
        varint.DecodeFromComparable(column_value.json_args(index).operand().value().varint_value());
      if (status.ok()) {
        array_index = VERIFY_RESULT(varint.ToInt64());
        return STATUS_SUBSTITUTE(QLError, "Cannot use array index $0 to access object",
            array_index);
      }
    }

    *last_elem_object = true;

    const auto& member = column_value.json_args(index).operand().value().string_value().c_str();
    *memberit = document->FindMember(member);
    if (*memberit == document->MemberEnd()) {
      return STATUS_SUBSTITUTE(QLError, "Could not find member: ", member);
    }
  } else {
    return STATUS_SUBSTITUTE(QLError, "JSON field is invalid", column_value.ShortDebugString());
  }
  return Status::OK();
}

CHECKED_STATUS CheckUserTimestampForCollections(const UserTimeMicros user_timestamp) {
  if (user_timestamp != Value::kInvalidUserTimestamp) {
    return STATUS(InvalidArgument, "User supplied timestamp is only allowed for "
        "replacing the whole collection");
  }
  return Status::OK();
}

} // namespace

QLWriteOperation::QLWriteOperation(std::shared_ptr<const Schema> schema,
                                   const IndexMap& index_map,
                                   const Schema* unique_index_key_schema,
                                   const TransactionOperationContextOpt& txn_op_context)
    : schema_(std::move(schema)),
      index_map_(index_map),
      unique_index_key_schema_(unique_index_key_schema),
      txn_op_context_(txn_op_context)
{}

Status QLWriteOperation::Init(QLWriteRequestPB* request, QLResponsePB* response) {
  request_.Swap(request);
  response_ = response;
  insert_into_unique_index_ = request_.type() == QLWriteRequestPB::QL_STMT_INSERT &&
                              unique_index_key_schema_ != nullptr;
  require_read_ = RequireRead(request_, *schema_) || insert_into_unique_index_;
  update_indexes_ = !request_.update_index_ids().empty();

  // Determine if static / non-static columns are being written.
  bool write_static_columns = false;
  bool write_non_static_columns = false;
  // TODO(Amit): Remove the DVLOGS after backfill features stabilize.
  DVLOG(4) << "Processing request " << yb::ToString(*request);
  for (const auto& column : request_.column_values()) {
    DVLOG(4) << "Looking at column : " << yb::ToString(column);
    auto schema_column = schema_->column_by_id(ColumnId(column.column_id()));
    DVLOG(4) << "schema column : " << yb::ToString(schema_column);
    RETURN_NOT_OK(schema_column);
    if (schema_column->is_static()) {
      write_static_columns = true;
    } else {
      write_non_static_columns = true;
    }
    if (write_static_columns && write_non_static_columns) {
      break;
    }
  }

  bool is_range_operation = IsRangeOperation(request_, *schema_);

  // We need the hashed key if writing to the static columns, and need primary key if writing to
  // non-static columns or writing the full primary key (i.e. range columns are present or table
  // does not have range columns).
  return InitializeKeys(
      write_static_columns || is_range_operation,
      write_non_static_columns || !request_.range_column_values().empty() ||
      schema_->num_range_key_columns() == 0);
}

Status QLWriteOperation::InitializeKeys(const bool hashed_key, const bool primary_key) {
  // Populate the hashed and range components in the same order as they are in the table schema.
  const auto& hashed_column_values = request_.hashed_column_values();
  const auto& range_column_values = request_.range_column_values();
  std::vector<PrimitiveValue> hashed_components;
  std::vector<PrimitiveValue> range_components;
  RETURN_NOT_OK(QLKeyColumnValuesToPrimitiveValues(
      hashed_column_values, *schema_, 0,
      schema_->num_hash_key_columns(), &hashed_components));
  RETURN_NOT_OK(QLKeyColumnValuesToPrimitiveValues(
      range_column_values, *schema_, schema_->num_hash_key_columns(),
      schema_->num_range_key_columns(), &range_components));

  // need_pk - true is we should construct pk_key_key_
  const bool need_pk = primary_key && !pk_doc_key_;

  // We need the hash key if writing to the static columns.
  if (hashed_key && !hashed_doc_key_) {
    if (need_pk) {
      hashed_doc_key_.emplace(request_.hash_code(), hashed_components);
    } else {
      hashed_doc_key_.emplace(request_.hash_code(), std::move(hashed_components));
    }
    encoded_hashed_doc_key_ = hashed_doc_key_->EncodeAsRefCntPrefix();
  }

  // We need the primary key if writing to non-static columns or writing the full primary key
  // (i.e. range columns are present).
  if (need_pk) {
    if (request_.has_hash_code() && !hashed_column_values.empty()) {
      pk_doc_key_.emplace(
         request_.hash_code(), std::move(hashed_components), std::move(range_components));
    } else {
      // In case of syscatalog tables, we don't have any hash components.
      pk_doc_key_.emplace(std::move(range_components));
    }
    encoded_pk_doc_key_ =  pk_doc_key_->EncodeAsRefCntPrefix();
  }

  return Status::OK();
}

Status QLWriteOperation::GetDocPaths(
    GetDocPathsMode mode, DocPathsToLock *paths, IsolationLevel *level) const {
  if (mode == GetDocPathsMode::kLock || request_.column_values().empty()) {
    if (encoded_hashed_doc_key_) {
      paths->push_back(encoded_hashed_doc_key_);
    }
    if (encoded_pk_doc_key_) {
      paths->push_back(encoded_pk_doc_key_);
    }
  } else {
    KeyBytes buffer;
    for (const auto& column_value : request_.column_values()) {
      ColumnId column_id(column_value.column_id());
      const ColumnSchema& column = VERIFY_RESULT(schema_->column_by_id(column_id));

      Slice doc_key = column.is_static() ? encoded_hashed_doc_key_.as_slice()
                                         : encoded_pk_doc_key_.as_slice();
      buffer.Clear();
      buffer.AppendValueType(ValueType::kColumnId);
      buffer.AppendColumnId(column_id);
      RefCntBuffer path(doc_key.size() + buffer.size());
      memcpy(path.data(), doc_key.data(), doc_key.size());
      buffer.AsSlice().CopyTo(path.data() + doc_key.size());
      paths->push_back(RefCntPrefix(path));
    }
  }

  // When this write operation requires a read, it requires a read snapshot so paths will be locked
  // in snapshot isolation for consistency. Otherwise, pure writes will happen in serializable
  // isolation so that they will serialize but do not conflict with one another.
  //
  // Currently, only keys that are being written are locked, no lock is taken on read at the
  // snapshot isolation level.
  *level = require_read_ ? IsolationLevel::SNAPSHOT_ISOLATION
                         : IsolationLevel::SERIALIZABLE_ISOLATION;

  return Status::OK();
}

Status QLWriteOperation::ReadColumns(const DocOperationApplyData& data,
                                     Schema *param_static_projection,
                                     Schema *param_non_static_projection,
                                     QLTableRow* table_row) {
  Schema *static_projection = param_static_projection;
  Schema *non_static_projection = param_non_static_projection;

  Schema local_static_projection;
  Schema local_non_static_projection;
  if (static_projection == nullptr) {
    static_projection = &local_static_projection;
  }
  if (non_static_projection == nullptr) {
    non_static_projection = &local_non_static_projection;
  }

  // Create projections to scan docdb.
  RETURN_NOT_OK(CreateProjections(*schema_, request_.column_refs(),
                                  static_projection, non_static_projection));

  // Generate hashed / primary key depending on if static / non-static columns are referenced in
  // the if-condition.
  RETURN_NOT_OK(InitializeKeys(
      !static_projection->columns().empty(), !non_static_projection->columns().empty()));

  // Scan docdb for the static and non-static columns of the row using the hashed / primary key.
  if (hashed_doc_key_) {
    DocQLScanSpec spec(*static_projection, *hashed_doc_key_, request_.query_id());
    DocRowwiseIterator iterator(*static_projection, *schema_, txn_op_context_,
                                data.doc_write_batch->doc_db(),
                                data.deadline, data.read_time);
    RETURN_NOT_OK(iterator.Init(spec));
    if (VERIFY_RESULT(iterator.HasNext())) {
      RETURN_NOT_OK(iterator.NextRow(table_row));
    }
    data.restart_read_ht->MakeAtLeast(iterator.RestartReadHt());
  }
  if (pk_doc_key_) {
    DocQLScanSpec spec(*non_static_projection, *pk_doc_key_, request_.query_id());
    DocRowwiseIterator iterator(*non_static_projection, *schema_, txn_op_context_,
                                data.doc_write_batch->doc_db(),
                                data.deadline, data.read_time);
    RETURN_NOT_OK(iterator.Init(spec));
    if (VERIFY_RESULT(iterator.HasNext())) {
      RETURN_NOT_OK(iterator.NextRow(table_row));
      // If there are indexes to update, check if liveness column exists for update/delete because
      // that will affect whether the row will still exist after the DML and whether we need to
      // remove the key from the indexes.
      if (update_indexes_ && (request_.type() == QLWriteRequestPB::QL_STMT_UPDATE ||
                              request_.type() == QLWriteRequestPB::QL_STMT_DELETE)) {
        liveness_column_exists_ = iterator.LivenessColumnExists();
      }
    } else {
      // If no non-static column is found, the row does not exist and we should clear the static
      // columns in the map to indicate the row does not exist.
      table_row->Clear();
    }
    data.restart_read_ht->MakeAtLeast(iterator.RestartReadHt());
  }

  return Status::OK();
}

Status QLWriteOperation::PopulateConditionalDmlRow(const DocOperationApplyData& data,
                                                   const bool should_apply,
                                                   const QLTableRow& table_row,
                                                   Schema static_projection,
                                                   Schema non_static_projection,
                                                   std::unique_ptr<QLRowBlock>* rowblock) {
  // Populate the result set to return the "applied" status, and optionally the hash / primary key
  // and the present column values if the condition is not satisfied and the row does exist
  // (value_map is not empty).
  const bool return_present_values = !should_apply && !table_row.IsEmpty();
  const size_t num_key_columns =
      pk_doc_key_ ? schema_->num_key_columns() : schema_->num_hash_key_columns();
  std::vector<ColumnSchema> columns;
  columns.emplace_back(ColumnSchema("[applied]", BOOL));
  if (return_present_values) {
    columns.insert(columns.end(), schema_->columns().begin(),
                   schema_->columns().begin() + num_key_columns);
    columns.insert(columns.end(), static_projection.columns().begin(),
                   static_projection.columns().end());
    columns.insert(columns.end(), non_static_projection.columns().begin(),
                   non_static_projection.columns().end());
  }
  rowblock->reset(new QLRowBlock(Schema(columns, 0)));
  QLRow& row = rowblock->get()->Extend();
  row.mutable_column(0)->set_bool_value(should_apply);
  size_t col_idx = 1;
  if (return_present_values) {
    RETURN_NOT_OK(PopulateRow(table_row, *schema_, 0, num_key_columns, &row, &col_idx));
    RETURN_NOT_OK(PopulateRow(table_row, static_projection, &row, &col_idx));
    RETURN_NOT_OK(PopulateRow(table_row, non_static_projection, &row, &col_idx));
  }

  return Status::OK();
}

Status QLWriteOperation::PopulateStatusRow(const DocOperationApplyData& data,
                                           const bool should_apply,
                                           const QLTableRow& table_row,
                                           std::unique_ptr<QLRowBlock>* rowblock) {
  std::vector<ColumnSchema> columns;
  columns.emplace_back(ColumnSchema("[applied]", BOOL));
  columns.emplace_back(ColumnSchema("[message]", STRING));
  columns.insert(columns.end(), schema_->columns().begin(), schema_->columns().end());

  rowblock->reset(new QLRowBlock(Schema(columns, 0)));
  QLRow& row = rowblock->get()->Extend();
  row.mutable_column(0)->set_bool_value(should_apply);
  // No message unless there is an error (then message will be set in executor).

  // If not applied report the existing row values as for regular if clause.
  if (!should_apply) {
    for (size_t i = 0; i < schema_->num_columns(); i++) {
      boost::optional<const QLValuePB&> col_val = table_row.GetValue(schema_->column_id(i));
      if (col_val.is_initialized()) {
        *(row.mutable_column(i + 2)) = *col_val;
      }
    }
  }

  return Status::OK();
}

// Check if a duplicate value is inserted into a unique index.
Result<bool> QLWriteOperation::HasDuplicateUniqueIndexValue(const DocOperationApplyData& data) {
  VLOG(3) << "Looking for collisions in\n"
          << docdb::DocDBDebugDumpToStr(data.doc_write_batch->doc_db());
  // We need to check backwards only for backfilled entries.
  bool ret =
      VERIFY_RESULT(HasDuplicateUniqueIndexValue(data, Direction::kForward)) ||
      (request_.is_backfill() &&
       VERIFY_RESULT(HasDuplicateUniqueIndexValue(data, Direction::kBackward)));
  if (!ret) {
    VLOG(3) << "No collisions found";
  }
  return ret;
}

Result<bool> QLWriteOperation::HasDuplicateUniqueIndexValue(
    const DocOperationApplyData& data, Direction direction) {
  VLOG(2) << "Looking for collision while going " << yb::ToString(direction)
          << ". Trying to insert " << *pk_doc_key_;
  auto requested_read_time = data.read_time;
  if (direction == Direction::kForward) {
    return HasDuplicateUniqueIndexValue(data, requested_read_time);
  }

  auto iter = CreateIntentAwareIterator(
      data.doc_write_batch->doc_db(),
      BloomFilterMode::USE_BLOOM_FILTER,
      pk_doc_key_->Encode().AsSlice(),
      request_.query_id(),
      txn_op_context_,
      data.deadline,
      ReadHybridTime::Max());

  HybridTime oldest_past_min_ht = VERIFY_RESULT(FindOldestOverwrittenTimestamp(
      iter.get(), SubDocKey(*pk_doc_key_), requested_read_time.read));
  const HybridTime oldest_past_min_ht_liveness =
      VERIFY_RESULT(FindOldestOverwrittenTimestamp(
          iter.get(),
          SubDocKey(*pk_doc_key_,
                    PrimitiveValue::SystemColumnId(SystemColumnIds::kLivenessColumn)),
          requested_read_time.read));
  oldest_past_min_ht.MakeAtMost(oldest_past_min_ht_liveness);
  if (!oldest_past_min_ht.is_valid()) {
    return false;
  }
  return HasDuplicateUniqueIndexValue(
      data, ReadHybridTime::SingleTime(oldest_past_min_ht));
}

Result<bool> QLWriteOperation::HasDuplicateUniqueIndexValue(
    const DocOperationApplyData& data, ReadHybridTime read_time) {
  // Set up the iterator to read the current primary key associated with the index key.
  DocQLScanSpec spec(*unique_index_key_schema_, *pk_doc_key_, request_.query_id(), true);
  DocRowwiseIterator iterator(
      *unique_index_key_schema_,
      *schema_,
      txn_op_context_,
      data.doc_write_batch->doc_db(),
      data.deadline,
      read_time);
  RETURN_NOT_OK(iterator.Init(spec));

  // It is a duplicate value if the index key exists already and the index value (corresponding to
  // the indexed table's primary key) is not the same.
  if (!VERIFY_RESULT(iterator.HasNext())) {
    VLOG(2) << "No collision found while checking at " << yb::ToString(read_time);
    return false;
  }
  QLTableRow table_row;
  RETURN_NOT_OK(iterator.NextRow(&table_row));
  std::unordered_set<ColumnId> key_column_ids(unique_index_key_schema_->column_ids().begin(),
                                              unique_index_key_schema_->column_ids().end());
  for (const auto& column_value : request_.column_values()) {
    ColumnId column_id(column_value.column_id());
    if (key_column_ids.count(column_id) > 0) {
      boost::optional<const QLValuePB&> existing_value = table_row.GetValue(column_id);
      const QLValuePB& new_value = column_value.expr().value();
      if (existing_value && *existing_value != new_value) {
        VLOG(2) << "Found collision while checking at " << yb::ToString(read_time)
                << "\nExisting: " << yb::ToString(*existing_value)
                << " vs New: " << yb::ToString(new_value)
                << "\nUsed read time as " << yb::ToString(data.read_time);
        DVLOG(3) << "DocDB is now:\n"
                 << docdb::DocDBDebugDumpToStr(data.doc_write_batch->doc_db());
        return true;
      }
    }
  }

  VLOG(2) << "No collision while checking at " << yb::ToString(read_time);
  return false;
}

Result<HybridTime> QLWriteOperation::FindOldestOverwrittenTimestamp(
    IntentAwareIterator* iter,
    const SubDocKey& sub_doc_key,
    HybridTime min_read_time) {
  HybridTime result;
  VLOG(3) << "Doing iter->Seek " << *pk_doc_key_;
  iter->Seek(*pk_doc_key_);
  if (iter->valid()) {
    const KeyBytes bytes = sub_doc_key.EncodeWithoutHt();
    const Slice& sub_key_slice = bytes.AsSlice();
    result = VERIFY_RESULT(
        iter->FindOldestRecord(sub_key_slice, min_read_time));
    VLOG(2) << "iter->FindOldestRecord returned " << result << " for "
            << SubDocKey::DebugSliceToString(sub_key_slice);
  } else {
    VLOG(3) << "iter->Seek " << *pk_doc_key_ << " turned out to be invalid";
  }
  return result;
}

Status QLWriteOperation::ApplyForJsonOperators(const QLColumnValuePB& column_value,
                                               const DocOperationApplyData& data,
                                               const DocPath& sub_path, const MonoDelta& ttl,
                                               const UserTimeMicros& user_timestamp,
                                               const ColumnSchema& column,
                                               QLTableRow* existing_row,
                                               bool is_insert) {
  using common::Jsonb;
  // Read the json column value inorder to perform a read modify write.
  QLExprResult temp;
  RETURN_NOT_OK(existing_row->ReadColumn(column_value.column_id(), temp.Writer()));
  const auto& ql_value = temp.Value();
  if (IsNull(ql_value)) {
    return STATUS_SUBSTITUTE(QLError, "Invalid Json value: ", column_value.ShortDebugString());
  }
  Jsonb jsonb(std::move(ql_value.jsonb_value()));
  rapidjson::Document document;
  RETURN_NOT_OK(jsonb.ToRapidJson(&document));

  // Deserialize the rhs.
  Jsonb rhs(std::move(column_value.expr().value().jsonb_value()));
  rapidjson::Document rhs_doc;
  RETURN_NOT_OK(rhs.ToRapidJson(&rhs_doc));

  // Update the json value.
  rapidjson::Value::MemberIterator memberit;
  rapidjson::Value::ValueIterator valueit;
  bool last_elem_object;
  rapidjson::Value* node = &document;

  int i = 0;
  auto status = FindMemberForIndex(column_value, i, node, &memberit, &valueit,
      &last_elem_object, is_insert);
  for (i = 1; i < column_value.json_args_size() && status.ok(); i++) {
    node = (last_elem_object) ? &(memberit->value) : &(*valueit);
    status = FindMemberForIndex(column_value, i, node, &memberit, &valueit,
        &last_elem_object, is_insert);
  }

  bool update_missing = false;
  if (is_insert) {
    RETURN_NOT_OK(status);
  } else {
    update_missing = !status.ok();
  }

  if (update_missing) {
    // NOTE: lhs path cannot exceed by more than one hop
    if (last_elem_object && i == column_value.json_args_size()) {
      auto val = column_value.json_args(i - 1).operand().value().string_value();
      rapidjson::Value v(val.c_str(), val.size(), document.GetAllocator());
      node->AddMember(v, rhs_doc, document.GetAllocator());
    } else {
      RETURN_NOT_OK(status);
    }
  } else if (last_elem_object) {
    memberit->value = rhs_doc.Move();
  } else {
    *valueit = rhs_doc.Move();
  }

  // Now write the new json value back.
  QLValue result;
  Jsonb jsonb_result;
  RETURN_NOT_OK(jsonb_result.FromRapidJson(document));
  *result.mutable_jsonb_value() = std::move(jsonb_result.MoveSerializedJsonb());
  const SubDocument& sub_doc =
      SubDocument::FromQLValuePB(result.value(), column.sorting_type(),
                                 yb::bfql::TSOpcode::kScalarInsert);
  RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
    sub_path, sub_doc, data.read_time, data.deadline,
    request_.query_id(), ttl, user_timestamp));

  // Update the current row as well so that we can accumulate the result of multiple json
  // operations and write the final value.
  existing_row->AllocColumn(column_value.column_id()).value = result.value();
  return Status::OK();
}

Status QLWriteOperation::ApplyForSubscriptArgs(const QLColumnValuePB& column_value,
                                               const QLTableRow& existing_row,
                                               const DocOperationApplyData& data,
                                               const MonoDelta& ttl,
                                               const UserTimeMicros& user_timestamp,
                                               const ColumnSchema& column,
                                               DocPath* sub_path) {
  QLExprResult expr_result;
  RETURN_NOT_OK(EvalExpr(column_value.expr(), existing_row, expr_result.Writer()));
  const yb::bfql::TSOpcode write_instr = GetTSWriteInstruction(column_value.expr());
  const SubDocument& sub_doc =
      SubDocument::FromQLValuePB(expr_result.Value(), column.sorting_type(), write_instr);
  RETURN_NOT_OK(CheckUserTimestampForCollections(user_timestamp));

  // Setting the value for a sub-column
  // Currently we only support two cases here: `map['key'] = v` and `list[index] = v`)
  // Any other case should be rejected by the semantic analyser before getting here
  // Later when we support frozen or nested collections this code may need refactoring
  DCHECK_EQ(column_value.subscript_args().size(), 1);
  DCHECK(column_value.subscript_args(0).has_value()) << "An index must be a constant";
  switch (column.type()->main()) {
    case MAP: {
      const PrimitiveValue &pv = PrimitiveValue::FromQLValuePB(
          column_value.subscript_args(0).value(),
          ColumnSchema::SortingType::kNotSpecified);
      sub_path->AddSubKey(pv);
      RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
          *sub_path, sub_doc, data.read_time, data.deadline,
          request_.query_id(), ttl, user_timestamp));
      break;
    }
    case LIST: {
      MonoDelta default_ttl = schema_->table_properties().HasDefaultTimeToLive() ?
          MonoDelta::FromMilliseconds(schema_->table_properties().DefaultTimeToLive()) :
          MonoDelta::kMax;

      // At YQL layer list indexes start at 0, but internally we start at 1.
      int index = column_value.subscript_args(0).value().int32_value() + 1;
      RETURN_NOT_OK(data.doc_write_batch->ReplaceCqlInList(
          *sub_path, {index}, {sub_doc}, data.read_time, data.deadline, request_.query_id(),
          default_ttl, ttl));
      break;
    }
    default: {
      LOG(ERROR) << "Unexpected type for setting subcolumn: "
                 << column.type()->ToString();
    }
  }
  return Status::OK();
}

Status QLWriteOperation::ApplyForRegularColumns(const QLColumnValuePB& column_value,
                                                const QLTableRow& existing_row,
                                                const DocOperationApplyData& data,
                                                const DocPath& sub_path, const MonoDelta& ttl,
                                                const UserTimeMicros& user_timestamp,
                                                const ColumnSchema& column,
                                                const ColumnId& column_id,
                                                QLTableRow* new_row) {
  using yb::bfql::TSOpcode;

  // Typical case, setting a columns value
  QLExprResult expr_result;
  RETURN_NOT_OK(EvalExpr(column_value.expr(), existing_row, expr_result.Writer()));
  const TSOpcode write_instr = GetTSWriteInstruction(column_value.expr());
  const SubDocument& sub_doc =
      SubDocument::FromQLValuePB(expr_result.Value(), column.sorting_type(), write_instr);
  switch (write_instr) {
    case TSOpcode::kToJson: FALLTHROUGH_INTENDED;
    case TSOpcode::kScalarInsert:
          RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
              sub_path, sub_doc, data.read_time, data.deadline,
              request_.query_id(), ttl, user_timestamp));
      break;
    case TSOpcode::kMapExtend:
    case TSOpcode::kSetExtend:
    case TSOpcode::kMapRemove:
    case TSOpcode::kSetRemove:
          RETURN_NOT_OK(CheckUserTimestampForCollections(user_timestamp));
          RETURN_NOT_OK(data.doc_write_batch->ExtendSubDocument(
            sub_path, sub_doc, data.read_time, data.deadline, request_.query_id(), ttl));
      break;
    case TSOpcode::kListPrepend:
          sub_doc.SetExtendOrder(ListExtendOrder::PREPEND_BLOCK);
          FALLTHROUGH_INTENDED;
    case TSOpcode::kListAppend:
          RETURN_NOT_OK(CheckUserTimestampForCollections(user_timestamp));
          RETURN_NOT_OK(data.doc_write_batch->ExtendList(
              sub_path, sub_doc, data.read_time, data.deadline, request_.query_id(), ttl));
      break;
    case TSOpcode::kListRemove:
      // TODO(akashnil or mihnea) this should call RemoveFromList once thats implemented
      // Currently list subtraction is computed in memory using builtin call so this
      // case should never be reached. Once it is implemented the corresponding case
      // from EvalQLExpressionPB should be uncommented to enable this optimization.
          RETURN_NOT_OK(CheckUserTimestampForCollections(user_timestamp));
          RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
              sub_path, sub_doc, data.read_time, data.deadline,
              request_.query_id(), ttl, user_timestamp));
      break;
    default:
      LOG(FATAL) << "Unsupported operation: " << static_cast<int>(write_instr);
      break;
  }

  if (update_indexes_) {
    new_row->AllocColumn(column_id, expr_result.Value());
  }
  return Status::OK();
}

Status QLWriteOperation::Apply(const DocOperationApplyData& data) {
  QLTableRow existing_row;
  if (request_.has_if_expr()) {
    // Check if the if-condition is satisfied.
    bool should_apply = true;
    Schema static_projection, non_static_projection;
    RETURN_NOT_OK(ReadColumns(data, &static_projection, &non_static_projection, &existing_row));
    RETURN_NOT_OK(EvalCondition(request_.if_expr().condition(), existing_row, &should_apply));
    // Set the response accordingly.
    response_->set_applied(should_apply);
    if (!should_apply && request_.else_error()) {
      return ql::ErrorStatus(ql::ErrorCode::CONDITION_NOT_SATISFIED); // QLError
    } else if (request_.returns_status()) {
      RETURN_NOT_OK(PopulateStatusRow(data, should_apply, existing_row, &rowblock_));
    } else {
      RETURN_NOT_OK(PopulateConditionalDmlRow(data,
          should_apply,
          existing_row,
          static_projection,
          non_static_projection,
          &rowblock_));
    }

    // If we do not need to apply we are already done.
    if (!should_apply) {
      response_->set_status(QLResponsePB::YQL_STATUS_OK);
      return Status::OK();
    }

    TEST_PAUSE_IF_FLAG(TEST_pause_write_apply_after_if);
  } else if (RequireReadForExpressions(request_) || request_.returns_status()) {
    RETURN_NOT_OK(ReadColumns(data, nullptr, nullptr, &existing_row));
    if (request_.returns_status()) {
      RETURN_NOT_OK(PopulateStatusRow(data, /* should_apply = */ true, existing_row, &rowblock_));
    }
  }

  VLOG(3) << "insert_into_unique_index_ is " << insert_into_unique_index_;
  if (insert_into_unique_index_ && VERIFY_RESULT(HasDuplicateUniqueIndexValue(data))) {
    VLOG(3) << "set_applied is set to " << false << " for over " << yb::ToString(existing_row);
    response_->set_applied(false);
    response_->set_status(QLResponsePB::YQL_STATUS_OK);
    return Status::OK();
  }

  const MonoDelta ttl =
      request_.has_ttl() ? MonoDelta::FromMilliseconds(request_.ttl()) : Value::kMaxTtl;

  const UserTimeMicros user_timestamp = request_.has_user_timestamp_usec() ?
      request_.user_timestamp_usec() : Value::kInvalidUserTimestamp;

  // Initialize the new row being written to either the existing row if read, or just populate
  // the primary key.
  QLTableRow new_row;
  if (!existing_row.IsEmpty()) {
    new_row = existing_row;
  } else {
    size_t idx = 0;
    for (const QLExpressionPB& expr : request_.hashed_column_values()) {
      new_row.AllocColumn(schema_->column_id(idx), expr.value());
      idx++;
    }
    for (const QLExpressionPB& expr : request_.range_column_values()) {
      new_row.AllocColumn(schema_->column_id(idx), expr.value());
      idx++;
    }
  }

  switch (request_.type()) {
    // QL insert == update (upsert) to be consistent with Cassandra's semantics. In either
    // INSERT or UPDATE, if non-key columns are specified, they will be inserted which will cause
    // the primary key to be inserted also when necessary. Otherwise, we should insert the
    // primary key at least.
    case QLWriteRequestPB::QL_STMT_INSERT:
    case QLWriteRequestPB::QL_STMT_UPDATE: {
      // Add the appropriate liveness column only for inserts.
      // We never use init markers for QL to ensure we perform writes without any reads to
      // ensure our write path is fast while complicating the read path a bit.
      auto is_insert = request_.type() == QLWriteRequestPB::QL_STMT_INSERT;
      if (is_insert && encoded_pk_doc_key_) {
        const DocPath sub_path(encoded_pk_doc_key_.as_slice(),
                               PrimitiveValue::SystemColumnId(SystemColumnIds::kLivenessColumn));
        const auto value = Value(PrimitiveValue(), ttl, user_timestamp);
        RETURN_NOT_OK(data.doc_write_batch->SetPrimitive(
            sub_path, value, data.read_time, data.deadline, request_.query_id()));
      }

      for (const auto& column_value : request_.column_values()) {
        if (!column_value.has_column_id()) {
          return STATUS_FORMAT(InvalidArgument, "column id missing: $0",
                               column_value.DebugString());
        }
        const ColumnId column_id(column_value.column_id());
        const auto maybe_column = schema_->column_by_id(column_id);
        RETURN_NOT_OK(maybe_column);
        const ColumnSchema& column = *maybe_column;

        DocPath sub_path(
            column.is_static() ?
                encoded_hashed_doc_key_.as_slice() : encoded_pk_doc_key_.as_slice(),
            PrimitiveValue(column_id));

        QLValue expr_result;
        if (!column_value.json_args().empty()) {
          RETURN_NOT_OK(ApplyForJsonOperators(column_value, data, sub_path, ttl,
                                              user_timestamp, column, &new_row, is_insert));
        } else if (!column_value.subscript_args().empty()) {
          RETURN_NOT_OK(ApplyForSubscriptArgs(column_value, existing_row, data, ttl,
                                              user_timestamp, column, &sub_path));
        } else {
          RETURN_NOT_OK(ApplyForRegularColumns(column_value, existing_row, data, sub_path, ttl,
                                               user_timestamp, column, column_id, &new_row));
        }
      }

      if (update_indexes_) {
        RETURN_NOT_OK(UpdateIndexes(existing_row, new_row));
      }
      break;
    }
    case QLWriteRequestPB::QL_STMT_DELETE: {
      // We have three cases:
      // 1. If non-key columns are specified, we delete only those columns.
      // 2. Otherwise, if range cols are missing, this must be a range delete.
      // 3. Otherwise, this is a normal delete.
      // Analyzer ensures these are the only cases before getting here (e.g. range deletes cannot
      // specify non-key columns).
      if (request_.column_values_size() > 0) {
        // Delete the referenced columns only.
        for (const auto& column_value : request_.column_values()) {
          CHECK(column_value.has_column_id())
              << "column id missing: " << column_value.DebugString();
          const ColumnId column_id(column_value.column_id());
          const auto& column = VERIFY_RESULT_REF(schema_->column_by_id(column_id));
          const DocPath sub_path(
              column.is_static() ?
                encoded_hashed_doc_key_.as_slice() : encoded_pk_doc_key_.as_slice(),
              PrimitiveValue(column_id));
          RETURN_NOT_OK(data.doc_write_batch->DeleteSubDoc(sub_path,
              data.read_time, data.deadline, request_.query_id(), user_timestamp));
          if (update_indexes_) {
            new_row.MarkTombstoned(column_id);
          }
        }
        if (update_indexes_) {
          RETURN_NOT_OK(UpdateIndexes(existing_row, new_row));
        }
      } else if (IsRangeOperation(request_, *schema_)) {
        // If the range columns are not specified, we read everything and delete all rows for
        // which the where condition matches.

        // Create the schema projection -- range deletes cannot reference non-primary key columns,
        // so the non-static projection is all we need, it should contain all referenced columns.
        Schema static_projection;
        Schema projection;
        RETURN_NOT_OK(CreateProjections(*schema_, request_.column_refs(),
            &static_projection, &projection));

        // Construct the scan spec basing on the WHERE condition.
        vector<PrimitiveValue> hashed_components;
        RETURN_NOT_OK(QLKeyColumnValuesToPrimitiveValues(
            request_.hashed_column_values(), *schema_, 0,
            schema_->num_hash_key_columns(), &hashed_components));

        boost::optional<int32_t> hash_code = request_.has_hash_code()
                                             ? boost::make_optional<int32_t>(request_.hash_code())
                                             : boost::none;
        const auto range_covers_whole_partition_key = !request_.has_where_expr();
        const auto include_static_columns_in_scan = range_covers_whole_partition_key &&
                                                    schema_->has_statics();
        DocQLScanSpec spec(*schema_,
                           hash_code,
                           hash_code, // max hash code.
                           hashed_components,
                           request_.has_where_expr() ? &request_.where_expr().condition() : nullptr,
                           nullptr,
                           request_.query_id(),
                           true /* is_forward_scan */,
                           include_static_columns_in_scan);

        // Create iterator.
        DocRowwiseIterator iterator(
            projection, *schema_, txn_op_context_,
            data.doc_write_batch->doc_db(),
            data.deadline, data.read_time);
        RETURN_NOT_OK(iterator.Init(spec));

        // Iterate through rows and delete those that match the condition.
        // TODO(mihnea): We do not lock here, so other write transactions coming in might appear
        // partially applied if they happen in the middle of a ranged delete.
        while (VERIFY_RESULT(iterator.HasNext())) {
          existing_row.Clear();
          RETURN_NOT_OK(iterator.NextRow(&existing_row));

          // Match the row with the where condition before deleting it.
          bool match = false;
          RETURN_NOT_OK(spec.Match(existing_row, &match));
          if (match) {
            const DocPath row_path(iterator.row_key());
            RETURN_NOT_OK(DeleteRow(row_path, data.doc_write_batch, data.read_time, data.deadline));
            if (update_indexes_) {
              liveness_column_exists_ = iterator.LivenessColumnExists();
              RETURN_NOT_OK(UpdateIndexes(existing_row, new_row));
            }
          }
        }
        data.restart_read_ht->MakeAtLeast(iterator.RestartReadHt());
      } else {
        // Otherwise, delete the referenced row (all columns).
        RETURN_NOT_OK(DeleteRow(DocPath(encoded_pk_doc_key_.as_slice()), data.doc_write_batch,
                                data.read_time, data.deadline));
        if (update_indexes_) {
          RETURN_NOT_OK(UpdateIndexes(existing_row, new_row));
        }
      }
      break;
    }
  }

  response_->set_status(QLResponsePB::YQL_STATUS_OK);

  return Status::OK();
}

Status QLWriteOperation::DeleteRow(const DocPath& row_path, DocWriteBatch* doc_write_batch,
                                   const ReadHybridTime& read_ht, const CoarseTimePoint deadline) {
  if (request_.has_user_timestamp_usec()) {
    // If user_timestamp is provided, we need to add a tombstone for each individual
    // column in the schema since we don't want to analyze this on the read path.
    for (int i = schema_->num_key_columns(); i < schema_->num_columns(); i++) {
      const DocPath sub_path(row_path.encoded_doc_key(),
                             PrimitiveValue(schema_->column_id(i)));
      RETURN_NOT_OK(doc_write_batch->DeleteSubDoc(sub_path,
                                                  read_ht,
                                                  deadline,
                                                  request_.query_id(),
                                                  request_.user_timestamp_usec()));
    }

    // Delete the liveness column as well.
    const DocPath liveness_column(
        row_path.encoded_doc_key(),
        PrimitiveValue::SystemColumnId(SystemColumnIds::kLivenessColumn));
    RETURN_NOT_OK(doc_write_batch->DeleteSubDoc(liveness_column,
                                                read_ht,
                                                deadline,
                                                request_.query_id(),
                                                request_.user_timestamp_usec()));
  } else {
    RETURN_NOT_OK(doc_write_batch->DeleteSubDoc(row_path, read_ht, deadline));
  }

  return Status::OK();
}

namespace {

YB_DEFINE_ENUM(ValueState, (kNull)(kNotNull)(kMissing));

ValueState GetValueState(const QLTableRow& row, const ColumnId column_id) {
  const auto value = row.GetValue(column_id);
  return !value ? ValueState::kMissing : IsNull(*value) ? ValueState::kNull : ValueState::kNotNull;
}

} // namespace

bool QLWriteOperation::IsRowDeleted(const QLTableRow& existing_row,
                                    const QLTableRow& new_row) const {
  // Delete the whole row?
  if (request_.type() == QLWriteRequestPB::QL_STMT_DELETE && request_.column_values().empty()) {
    return true;
  }

  // For update/delete, if there is no liveness column, the row will be deleted after the DML unless
  // a non-null column still remains.
  if ((request_.type() == QLWriteRequestPB::QL_STMT_UPDATE ||
       request_.type() == QLWriteRequestPB::QL_STMT_DELETE) &&
      !liveness_column_exists_) {
    for (size_t idx = schema_->num_key_columns(); idx < schema_->num_columns(); idx++) {
      if (schema_->column(idx).is_static()) {
        continue;
      }
      const ColumnId column_id = schema_->column_id(idx);
      switch (GetValueState(new_row, column_id)) {
        case ValueState::kNull: continue;
        case ValueState::kNotNull: return false;
        case ValueState::kMissing: break;
      }
      switch (GetValueState(existing_row, column_id)) {
        case ValueState::kNull: continue;
        case ValueState::kNotNull: return false;
        case ValueState::kMissing: break;
      }
    }
    return true;
  }

  return false;
}

namespace {

QLExpressionPB* NewKeyColumn(QLWriteRequestPB* request, const IndexInfo& index, const size_t idx) {
  return (idx < index.hash_column_count()
          ? request->add_hashed_column_values()
          : request->add_range_column_values());
}

QLWriteRequestPB* NewIndexRequest(
    const IndexInfo& index,
    QLWriteRequestPB::QLStmtType type,
    vector<pair<const IndexInfo*, QLWriteRequestPB>>* index_requests) {
  index_requests->emplace_back(&index, QLWriteRequestPB());
  QLWriteRequestPB* const request = &index_requests->back().second;
  request->set_type(type);
  return request;
}

} // namespace

Status QLWriteOperation::UpdateIndexes(const QLTableRow& existing_row, const QLTableRow& new_row) {
  // Prepare the write requests to update the indexes. There should be at most 2 requests for each
  // index (one insert and one delete).
  VLOG(2) << "Updating indexes";
  const auto& index_ids = request_.update_index_ids();
  index_requests_.reserve(index_ids.size() * 2);
  for (const TableId& index_id : index_ids) {
    const IndexInfo* index = VERIFY_RESULT(index_map_.FindIndex(index_id));
    bool index_key_changed = false;
    if (IsRowDeleted(existing_row, new_row)) {
      index_key_changed = true;
    } else {
      VERIFY_RESULT(CreateAndSetupIndexInsertRequest(
          this, index->HasWritePermission(), existing_row, new_row, index,
          &index_requests_, &index_key_changed));
    }

    // If the index key is changed, delete the current key.
    if (index_key_changed && index->HasDeletePermission()) {
      QLWriteRequestPB* const index_request =
          NewIndexRequest(*index, QLWriteRequestPB::QL_STMT_DELETE, &index_requests_);
      for (size_t idx = 0; idx < index->key_column_count(); idx++) {
        const IndexInfo::IndexColumn& index_column = index->column(idx);
        QLExpressionPB *key_column = NewKeyColumn(index_request, *index, idx);

        // For old message expr_case() == NOT SET.
        // For new message expr_case == kColumnId when indexing expression is a column-ref.
        if (index_column.colexpr.expr_case() != QLExpressionPB::ExprCase::EXPR_NOT_SET &&
            index_column.colexpr.expr_case() != QLExpressionPB::ExprCase::kColumnId) {
          QLExprResult result;
          RETURN_NOT_OK(EvalExpr(index_column.colexpr, existing_row, result.Writer()));
          result.MoveTo(key_column->mutable_value());
        } else {
          auto result = existing_row.GetValue(index_column.indexed_column_id);
          if (result) {
            key_column->mutable_value()->CopyFrom(*result);
          }
        }
      }
    }
  }

  return Status::OK();
}

Result<QLWriteRequestPB*> CreateAndSetupIndexInsertRequest(
    QLExprExecutor* expr_executor,
    bool index_has_write_permission,
    const QLTableRow& existing_row,
    const QLTableRow& new_row,
    const IndexInfo* index,
    vector<pair<const IndexInfo*, QLWriteRequestPB>>* index_requests,
    bool* has_index_key_changed) {
  bool index_key_changed = false;
  bool update_this_index = false;
  unordered_map<size_t, QLValuePB> values;

  // Prepare the new index key.
  for (size_t idx = 0; idx < index->key_column_count(); idx++) {
    const IndexInfo::IndexColumn& index_column = index->column(idx);
    bool column_changed = true;

    // Column_id should be used without executing "colexpr" for the following cases (we want
    // to avoid executing colexpr as it is less efficient).
    // - Old PROTO messages (expr_case() == NOT SET).
    // - When indexing expression is just a column-ref (expr_case == kColumnId)
    if (index_column.colexpr.expr_case() == QLExpressionPB::ExprCase::EXPR_NOT_SET ||
        index_column.colexpr.expr_case() == QLExpressionPB::ExprCase::kColumnId) {
      auto result = new_row.GetValue(index_column.indexed_column_id);
      if (!existing_row.IsEmpty()) {
        // For each column in the index key, if there is a new value, see if the value is
        // changed from the current value. Else, use the current value.
        if (result) {
          if (new_row.MatchColumn(index_column.indexed_column_id, existing_row)) {
            column_changed = false;
          } else {
            index_key_changed = true;
          }
        } else {
          result = existing_row.GetValue(index_column.indexed_column_id);
        }
      }
      if (result) {
        values[idx] = std::move(*result);
      }
    } else {
      QLExprResult result;
      if (existing_row.IsEmpty()) {
        RETURN_NOT_OK(expr_executor->EvalExpr(index_column.colexpr, new_row, result.Writer()));
      } else {
        // For each column in the index key, if there is a new value, see if the value is
        // specified in the new value. Otherwise, use the current value.
        if (new_row.IsColumnSpecified(index_column.indexed_column_id)) {
          RETURN_NOT_OK(expr_executor->EvalExpr(index_column.colexpr, new_row, result.Writer()));
          // Compare new and existing results of the expression, if the results are equal
          // that means the key is NOT changed in fact even if the column value is changed.
          QLExprResult existing_result;
          RETURN_NOT_OK(expr_executor->EvalExpr(
              index_column.colexpr, existing_row, existing_result.Writer()));
          if (result.Value() == existing_result.Value()) {
            column_changed = false;
          } else {
            index_key_changed = true;
          }
        } else {
          RETURN_NOT_OK(expr_executor->EvalExpr(
              index_column.colexpr, existing_row, result.Writer()));
        }
      }

      result.MoveTo(&values[idx]);
    }

    if (column_changed) {
      update_this_index = true;
    }
  }

  // Prepare the covering columns.
  for (size_t idx = index->key_column_count(); idx < index->columns().size(); idx++) {
    const IndexInfo::IndexColumn& index_column = index->column(idx);
    auto result = new_row.GetValue(index_column.indexed_column_id);
    bool column_changed = true;

    // If the index value is changed and there is no new covering column value set, use the
    // current value.
    if (index_key_changed) {
      if (!result) {
        result = existing_row.GetValue(index_column.indexed_column_id);
      }
    } else if (!FLAGS_ycql_disable_index_updating_optimization &&
        result && new_row.MatchColumn(index_column.indexed_column_id, existing_row)) {
      column_changed = false;
    }
    if (result) {
      values[idx] = std::move(*result);
    }

    if (column_changed) {
      update_this_index = true;
    }
  }

  if (has_index_key_changed) {
    *has_index_key_changed = index_key_changed;
  }

  if (index_has_write_permission &&
      (update_this_index || FLAGS_ycql_disable_index_updating_optimization)) {
    QLWriteRequestPB* const index_request =
        NewIndexRequest(*index, QLWriteRequestPB::QL_STMT_INSERT, index_requests);

    // Setup the key columns.
    for (size_t idx = 0; idx < index->key_column_count(); idx++) {
      QLExpressionPB* const key_column = NewKeyColumn(index_request, *index, idx);
      auto it = values.find(idx);
      if (it != values.end()) {
        *key_column->mutable_value() = std::move(it->second);
      }
    }

    // Setup the covering columns.
    for (size_t idx = index->key_column_count(); idx < index->columns().size(); idx++) {
      auto it = values.find(idx);
      if (it != values.end()) {
        const IndexInfo::IndexColumn& index_column = index->column(idx);
        QLColumnValuePB* const covering_column = index_request->add_column_values();
        covering_column->set_column_id(index_column.column_id);
        *covering_column->mutable_expr()->mutable_value() = std::move(it->second);
      }
    }

    return index_request;
  }

  return nullptr; // The index updating was skipped.
}

Status QLReadOperation::Execute(const common::YQLStorageIf& ql_storage,
                                CoarseTimePoint deadline,
                                const ReadHybridTime& read_time,
                                const Schema& schema,
                                const Schema& projection,
                                QLResultSet* resultset,
                                HybridTime* restart_read_ht) {
  SimulateTimeoutIfTesting(&deadline);
  size_t row_count_limit = std::numeric_limits<std::size_t>::max();
  size_t num_rows_skipped = 0;
  size_t offset = 0;
  if (request_.has_offset()) {
    offset = request_.offset();
  }
  if (request_.has_limit()) {
    if (request_.limit() == 0) {
      return Status::OK();
    }
    row_count_limit = request_.limit();
  }

  // Create the projections of the non-key columns selected by the row block plus any referenced in
  // the WHERE condition. When DocRowwiseIterator::NextRow() populates the value map, it uses this
  // projection only to scan sub-documents. The query schema is used to select only referenced
  // columns and key columns.
  Schema static_projection, non_static_projection;
  RETURN_NOT_OK(CreateProjections(schema, request_.column_refs(),
                                  &static_projection, &non_static_projection));
  const bool read_static_columns = !static_projection.columns().empty();
  const bool read_distinct_columns = request_.distinct();

  std::unique_ptr<common::YQLRowwiseIteratorIf> iter;
  std::unique_ptr<common::QLScanSpec> spec, static_row_spec;
  RETURN_NOT_OK(ql_storage.BuildYQLScanSpec(
      request_, read_time, schema, read_static_columns, static_projection, &spec,
      &static_row_spec));
  RETURN_NOT_OK(ql_storage.GetIterator(request_, projection, schema, txn_op_context_,
                                       deadline, read_time, *spec, &iter));
  if (FLAGS_trace_docdb_calls) {
    TRACE("Initialized iterator");
  }

  QLTableRow static_row;
  QLTableRow non_static_row;
  QLTableRow& selected_row = read_distinct_columns ? static_row : non_static_row;

  // In case when we are continuing a select with a paging state, or when using a reverse scan,
  // the static columns for the next row to fetch are not included in the first iterator and we
  // need to fetch them with a separate spec and iterator before beginning the normal fetch below.
  if (static_row_spec != nullptr) {
    std::unique_ptr<common::YQLRowwiseIteratorIf> static_row_iter;
    RETURN_NOT_OK(ql_storage.GetIterator(
        request_, static_projection, schema, txn_op_context_, deadline, read_time,
        *static_row_spec, &static_row_iter));
    if (VERIFY_RESULT(static_row_iter->HasNext())) {
      RETURN_NOT_OK(static_row_iter->NextRow(&static_row));
    }
  }

  // Begin the normal fetch.
  int match_count = 0;
  bool static_dealt_with = true;
  while (resultset->rsrow_count() < row_count_limit && VERIFY_RESULT(iter->HasNext())) {
    const bool last_read_static = iter->IsNextStaticColumn();

    // Note that static columns are sorted before non-static columns in DocDB as follows. This is
    // because "<empty_range_components>" is empty and terminated by kGroupEnd which sorts before
    // all other ValueType characters in a non-empty range component.
    //   <hash_code><hash_components><empty_range_components><static_column_id> -> value;
    //   <hash_code><hash_components><range_components><non_static_column_id> -> value;
    if (last_read_static) {
      static_row.Clear();
      RETURN_NOT_OK(iter->NextRow(static_projection, &static_row));
    } else { // Reading a regular row that contains non-static columns.
      // Read this regular row.
      // TODO(omer): this is quite inefficient if read_distinct_column. A better way to do this
      // would be to only read the first non-static column for each hash key, and skip the rest
      non_static_row.Clear();
      RETURN_NOT_OK(iter->NextRow(non_static_projection, &non_static_row));
    }

    // We have two possible cases: whether we use distinct or not
    // If we use distinct, then in general we only need to add the static rows
    // However, we might have to add non-static rows, if there is no static row corresponding to
    // it. Of course, we add one entry per hash key in non-static row.
    // If we do not use distinct, we are generally only adding non-static rows
    // However, if there is no non-static row for the static row, we have to add it.
    if (read_distinct_columns) {
      bool join_successful = false;
      if (!last_read_static) {
        join_successful = JoinNonStaticRow(schema, static_projection, non_static_row, &static_row);
      }

      // If the join was not successful, it means that the non-static row we read has no
      // corresponding static row, so we have to add it to the result
      if (!join_successful) {
        RETURN_NOT_OK(AddRowToResult(
            spec, static_row, row_count_limit, offset, resultset, &match_count, &num_rows_skipped));
      }
    } else {
      if (last_read_static) {
        // If the next row to be read is not static, deal with it later, as we do not know whether
        // the non-static row corresponds to this static row; if the non-static row doesn't
        // correspond to this static row, we will have to add it later, so set static_dealt_with to
        // false
        if (VERIFY_RESULT(iter->HasNext()) && !iter->IsNextStaticColumn()) {
          static_dealt_with = false;
          continue;
        }

        AddProjection(non_static_projection, &static_row);
        RETURN_NOT_OK(AddRowToResult(spec, static_row, row_count_limit, offset, resultset,
                                     &match_count, &num_rows_skipped));
      } else {
        // We also have to do the join if we are not reading any static columns, as Cassandra
        // reports nulls for static rows with no corresponding non-static row
        if (read_static_columns || !static_dealt_with) {
          const bool join_successful = JoinStaticRow(schema,
                                               static_projection,
                                               static_row,
                                               &non_static_row);
          // Add the static row if the join was not successful and it is the first time we are
          // dealing with this static row
          if (!join_successful && !static_dealt_with) {
            AddProjection(non_static_projection, &static_row);
            RETURN_NOT_OK(AddRowToResult(
                spec, static_row, row_count_limit, offset, resultset, &match_count,
                &num_rows_skipped));
          }
        }
        static_dealt_with = true;
        RETURN_NOT_OK(AddRowToResult(
            spec, non_static_row, row_count_limit, offset, resultset, &match_count,
            &num_rows_skipped));
      }
    }
  }

  if (request_.is_aggregate() && match_count > 0) {
    RETURN_NOT_OK(PopulateAggregate(selected_row, resultset));
  }

  if (FLAGS_trace_docdb_calls) {
    TRACE("Fetched $0 rows.", resultset->rsrow_count());
  }

  RETURN_NOT_OK(SetPagingStateIfNecessary(
      iter.get(), resultset, row_count_limit, num_rows_skipped, read_time));

  // SetPagingStateIfNecessary could perform read, so we assign restart_read_ht after it.
  *restart_read_ht = iter->RestartReadHt();

  return Status::OK();
}

Status QLReadOperation::SetPagingStateIfNecessary(const common::YQLRowwiseIteratorIf* iter,
                                                  const QLResultSet* resultset,
                                                  const size_t row_count_limit,
                                                  const size_t num_rows_skipped,
                                                  const ReadHybridTime& read_time) {
  if ((resultset->rsrow_count() >= row_count_limit || request_.has_offset()) &&
      !request_.is_aggregate()) {
    SubDocKey next_row_key;
    RETURN_NOT_OK(iter->GetNextReadSubDocKey(&next_row_key));
    // When the "limit" number of rows are returned and we are asked to return the paging state,
    // return the partition key and row key of the next row to read in the paging state if there are
    // still more rows to read. Otherwise, leave the paging state empty which means we are done
    // reading from this tablet.
    if (request_.return_paging_state()) {
      if (!next_row_key.doc_key().empty()) {
        QLPagingStatePB* paging_state = response_.mutable_paging_state();
        paging_state->set_next_partition_key(
            PartitionSchema::EncodeMultiColumnHashValue(next_row_key.doc_key().hash()));
        paging_state->set_next_row_key(next_row_key.Encode().ToStringBuffer());
        paging_state->set_total_rows_skipped(request_.paging_state().total_rows_skipped() +
            num_rows_skipped);
      } else if (request_.has_offset()) {
        QLPagingStatePB* paging_state = response_.mutable_paging_state();
        paging_state->set_total_rows_skipped(request_.paging_state().total_rows_skipped() +
            num_rows_skipped);
      }
    }
    if (response_.has_paging_state()) {
      if (FLAGS_ycql_consistent_transactional_paging) {
        read_time.AddToPB(response_.mutable_paging_state());
      } else {
        // Using SingleTime will help avoid read restarts on second page and later but will
        // potentially produce stale results on those pages.
        auto per_row_consistent_read_time = ReadHybridTime::SingleTime(read_time.read);
        per_row_consistent_read_time.AddToPB(response_.mutable_paging_state());
      }
    }
  }

  return Status::OK();
}

Status QLReadOperation::GetIntents(const Schema& schema, KeyValueWriteBatchPB* out) {
  std::vector<PrimitiveValue> hashed_components;
  RETURN_NOT_OK(QLKeyColumnValuesToPrimitiveValues(
      request_.hashed_column_values(), schema, 0, schema.num_hash_key_columns(),
      &hashed_components));
  auto pair = out->mutable_read_pairs()->Add();
  if (hashed_components.empty()) {
    // Empty hashed components mean that we don't have primary key at all, but request
    // could still contain hash_code as part of tablet routing.
    // So we should ignore it.
    pair->set_key(std::string(1, ValueTypeAsChar::kGroupEnd));
  } else {
    DocKey doc_key(request_.hash_code(), hashed_components);
    pair->set_key(doc_key.Encode().ToStringBuffer());
  }
  pair->set_value(std::string(1, ValueTypeAsChar::kNullLow));
  return Status::OK();
}

Status QLReadOperation::PopulateResultSet(const std::unique_ptr<common::QLScanSpec>& spec,
                                          const QLTableRow& table_row,
                                          QLResultSet *resultset) {
  resultset->AllocateRow();
  int rscol_index = 0;
  for (const QLExpressionPB& expr : request_.selected_exprs()) {
    QLExprResult value;
    RETURN_NOT_OK(EvalExpr(expr, table_row, value.Writer(), spec->schema()));
    resultset->AppendColumn(rscol_index, value.Value());
    rscol_index++;
  }

  return Status::OK();
}

Status QLReadOperation::EvalAggregate(const QLTableRow& table_row) {
  if (aggr_result_.empty()) {
    int column_count = request_.selected_exprs().size();
    aggr_result_.resize(column_count);
  }

  int aggr_index = 0;
  for (const QLExpressionPB& expr : request_.selected_exprs()) {
    RETURN_NOT_OK(EvalExpr(expr, table_row, aggr_result_[aggr_index++].Writer()));
  }
  return Status::OK();
}

Status QLReadOperation::PopulateAggregate(const QLTableRow& table_row, QLResultSet *resultset) {
  resultset->AllocateRow();
  int column_count = request_.selected_exprs().size();
  for (int rscol_index = 0; rscol_index < column_count; rscol_index++) {
    resultset->AppendColumn(rscol_index, aggr_result_[rscol_index].Value());
  }
  return Status::OK();
}

Status QLReadOperation::AddRowToResult(const std::unique_ptr<common::QLScanSpec>& spec,
                                       const QLTableRow& row,
                                       const size_t row_count_limit,
                                       const size_t offset,
                                       QLResultSet* resultset,
                                       int* match_count,
                                       size_t *num_rows_skipped) {
  VLOG(3) << __FUNCTION__ << " : " << yb::ToString(row);
  if (resultset->rsrow_count() < row_count_limit) {
    bool match = false;
    RETURN_NOT_OK(spec->Match(row, &match));
    if (match) {
      if (*num_rows_skipped >= offset) {
        (*match_count)++;
        if (request_.is_aggregate()) {
          RETURN_NOT_OK(EvalAggregate(row));
        } else {
          RETURN_NOT_OK(PopulateResultSet(spec, row, resultset));
        }
      } else {
        (*num_rows_skipped)++;
      }
    }
  }
  return Status::OK();
}

}  // namespace docdb
}  // namespace yb
