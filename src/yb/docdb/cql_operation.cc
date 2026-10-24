// Copyright (c) YugabyteDB, Inc.
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

#include "yb/bfpg/tserver_opcodes.h"

#include "yb/common/json_util.h"
#include "yb/common/jsonb.h"
#include "yb/common/ql_protocol.messages.h"
#include "yb/common/ql_protocol_util.h"
#include "yb/common/ql_value.h"
#include "yb/common/value.messages.h"

#include "yb/docdb/doc_ql_scanspec.h"
#include "yb/docdb/doc_read_context.h"
#include "yb/docdb/doc_rowwise_iterator.h"
#include "yb/docdb/doc_write_batch.h"
#include "yb/docdb/docdb.messages.h"
#include "yb/docdb/docdb_debug.h"
#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/docdb/ql_storage_interface.h"

#include "yb/dockv/doc_path.h"
#include "yb/dockv/packed_row.h"
#include "yb/dockv/partition.h"
#include "yb/dockv/primitive_value_util.h"
#include "yb/dockv/reader_projection.h"

#include "yb/qlexpr/index.h"
#include "yb/qlexpr/index_column.h"
#include "yb/qlexpr/ql_resultset.h"
#include "yb/qlexpr/ql_rowblock.h"

#include "yb/util/debug-util.h"
#include "yb/util/flags.h"
#include "yb/util/logging.h"
#include "yb/util/range.h"
#include "yb/util/result.h"
#include "yb/util/scope_exit.h"
#include "yb/util/status.h"
#include "yb/util/status_format.h"
#include "yb/util/trace.h"

#include "yb/yql/cql/ql/util/errcodes.h"

DEFINE_test_flag(bool, pause_write_apply_after_if, false,
                 "Pause application of QLWriteOperation after evaluating if condition.");

DEFINE_UNKNOWN_bool(ycql_consistent_transactional_paging, false,
            "Whether to enforce consistency of data returned for second page and beyond for YCQL "
            "queries on transactional tables. If true, read restart errors could be returned to "
            "prevent inconsistency. If false, no read restart errors are returned but the data may "
            "be stale. The latter is preferable for long scans. The data returned for the first "
            "page of results is never stale regardless of this flag.");

DEFINE_UNKNOWN_bool(ycql_disable_index_updating_optimization, false,
            "If true all secondary indexes must be updated even if the update does not change "
            "the index data.");
TAG_FLAG(ycql_disable_index_updating_optimization, advanced);

#ifdef NDEBUG
constexpr bool kYcqlPackedRowEnabled = false;
#else
constexpr bool kYcqlPackedRowEnabled = true;
#endif

DEFINE_RUNTIME_bool(ycql_enable_packed_row, kYcqlPackedRowEnabled,
                    "Whether packed row is enabled for YCQL.");

DEFINE_RUNTIME_bool(ycql_jsonb_use_member_cache, true,
                    "Whether we use member cache during jsonb processing in YCQL.");

DEFINE_UNKNOWN_uint64(ycql_packed_row_size_limit, 0,
    "Packed row size limit for YCQL in bytes. 0 to make this equal to SSTable block size.");

namespace yb {
namespace docdb {

using std::unordered_map;
using std::unordered_set;
using std::vector;
using dockv::DocPath;
using dockv::KeyEntryValue;
using dockv::ValueControlFields;

using qlexpr::QLExprResult;
using qlexpr::QLResultSet;
using qlexpr::QLRow;
using qlexpr::QLTableRow;

namespace {

// Append dummy entries in schema to table_row
// TODO(omer): this should most probably be added somewhere else
void AddProjection(const dockv::ReaderProjection& projection, QLTableRow* table_row) {
  for (const auto& column : projection.value_columns()) {
    table_row->AllocColumn(column.id);
  }
}

YB_DEFINE_ENUM(AddKeysMode, (kNone)(kHash)(kAll));

// Create projection schemas of static and non-static columns from table schema.
// Used to fetch only columns that were requested by the user.
// add_keys_mode - what key columns should be added to all projections even they were not specified.
template <class ColumnsPB>
Status CreateProjections(
    const Schema& schema, const ColumnsPB& column_refs,
    dockv::ReaderProjection* static_projection, dockv::ReaderProjection* non_static_projection,
    dockv::ReaderProjection* full_projection, AddKeysMode add_keys_mode) {

  if (schema.has_statics() && add_keys_mode == AddKeysMode::kNone) {
    add_keys_mode = AddKeysMode::kHash;
  }

  if (add_keys_mode == AddKeysMode::kNone) {
    non_static_projection->Init(schema, column_refs.ids());
    if (full_projection) {
      *full_projection = *non_static_projection;
    }
    return Status::OK();
  }

  static_projection->Init(schema, column_refs.static_ids(), schema.hash_key_column_ids());

  auto extra_columns = add_keys_mode == AddKeysMode::kHash
      ? schema.hash_key_column_ids() : schema.key_column_ids();
  non_static_projection->Init(schema, column_refs.ids(), extra_columns);

  if (full_projection) {
    full_projection->Init(schema, column_refs.static_ids(), column_refs.ids(), extra_columns);
  }

  return Status::OK();
}

// Outer join a static row with a non-static row.
// A join is successful if and only if for every hash key, the values in the static and the
// non-static row are either non-NULL and the same, or one of them is NULL. Therefore we say that
// a join is successful if the static row is empty, and in turn return true.
// Copies the entries from the static row into the non-static one.
bool JoinStaticRow(
    const Schema& schema, const dockv::ReaderProjection& static_projection,
    const QLTableRow& static_row, QLTableRow* non_static_row) {
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
  for (const auto& column : static_projection.columns) {
    non_static_row->CopyColumn(column.id, static_row);
  }

  return true;
}

// Join a non-static row with a static row.
// Returns true if the two rows match
bool JoinNonStaticRow(
    const Schema& schema, const dockv::ReaderProjection& static_projection,
    const QLTableRow& non_static_row, QLTableRow* static_row) {
  bool join_successful = true;

  for (size_t i = 0; i < schema.num_hash_key_columns(); i++) {
    if (!static_row->MatchColumn(schema.column_id(i), non_static_row)) {
      join_successful = false;
      break;
    }
  }

  if (!join_successful) {
    static_row->Clear();
    for (const auto& column : static_projection.columns) {
      static_row->AllocColumn(column.id);
    }

    for (size_t i = 0; i < schema.num_hash_key_columns(); i++) {
      static_row->CopyColumn(schema.column_id(i), non_static_row);
    }
  }
  return join_successful;
}

typedef rapidjson::SizeType RapidJsonMemberIndex;
using MembersMap = UnorderedStringMap<std::string, RapidJsonMemberIndex>;
using RapidJsonMemberCache = std::unordered_map<uintptr_t, MembersMap>;

uintptr_t MemberCacheValueHash(rapidjson::Value* p) { return reinterpret_cast<uintptr_t>(p); }

Status FindMemberForIndex(const QLColumnValueMsg& column_value,
                          int index,
                          rapidjson::Value* document,
                          rapidjson::Value::MemberIterator* memberit,
                          rapidjson::Value::ValueIterator* valueit,
                          bool* last_elem_object,
                          IsInsert is_insert,
                          RapidJsonMemberCache* member_cache) {
  *last_elem_object = false;

  int64_t array_index;
  auto it = std::next(column_value.json_args().begin(), index);
  if (document->IsArray()) {
    VarInt varint;
    RETURN_NOT_OK(varint.DecodeFromComparable(it->operand().value().varint_value()));
    array_index = VERIFY_RESULT(varint.ToInt64());

    if (array_index >= document->GetArray().Size() || array_index < 0) {
      return STATUS_FORMAT(QLError, "Array index out of bounds: ", array_index);
    }
    *valueit = document->Begin();
    std::advance(*valueit, array_index);
  } else if (document->IsObject()) {
    if (!is_insert) {
      VarInt varint;
      auto status = varint.DecodeFromComparable(it->operand().value().varint_value());
      if (status.ok()) {
        array_index = VERIFY_RESULT(varint.ToInt64());
        return STATUS_FORMAT(QLError, "Cannot use array index $0 to access object",
            array_index);
      }
    }

    *last_elem_object = true;

    const auto& member = it->operand().value().string_value();
    *memberit = document->MemberEnd();

    if (FLAGS_ycql_jsonb_use_member_cache) {
      auto node_itr = member_cache->find(MemberCacheValueHash(document));
      if (node_itr != member_cache->end()) {
        // Find the member from the cache.
        auto member_itr = node_itr->second.find(member);
        if (member_itr != node_itr->second.end()) {
          LOG_IF(DFATAL, member_itr->second >= document->MemberCount())
              << "Invalid index in the member cache";
          *memberit = document->MemberBegin() + member_itr->second;
        }
      } else {
        // Fetch and cache all the members of the document.
        MembersMap members;
        RapidJsonMemberIndex member_idx = 0;
        for (auto m = document->MemberBegin(); m != document->MemberEnd(); m++, member_idx++) {
          auto curr_member_name = m->name.GetString();
          if (curr_member_name == member) {
            LOG_IF(DFATAL, !(*memberit == document->MemberEnd()))
                << "Duplicate member found in the json object";
            *memberit = m;
          }
          members[curr_member_name] = member_idx;
        }
        (*member_cache)[MemberCacheValueHash(document)] = std::move(members);
      }
    } else {
      rapidjson::Value member_value(rapidjson::StringRef(member.cdata(), member.size()));
      *memberit = document->FindMember(member_value);
    }

    if (memberit->operator==(document->MemberEnd())) {
      return STATUS_FORMAT(QLError, "Could not find member: ", member);
    }
  } else {
    return STATUS_FORMAT(QLError, "JSON field is invalid", column_value.ShortDebugString());
  }
  return Status::OK();
}

Status CheckUserTimestampForCollections(const UserTimeMicros user_timestamp) {
  if (user_timestamp != ValueControlFields::kInvalidTimestamp) {
    return STATUS(InvalidArgument, "User supplied timestamp is only allowed for "
        "replacing the whole collection");
  }
  return Status::OK();
}

} // namespace

struct QLWriteOperation::ApplyContext {
  const DocOperationApplyData* data;
  ValueControlFields control_fields;
  ValueBuffer column_control_fields;
  dockv::RowPackerV1* row_packer = nullptr;
};

QLWriteOperation::QLWriteOperation(
    std::reference_wrapper<const QLWriteRequestMsg> request,
    SchemaVersion schema_version,
    DocReadContextPtr doc_read_context,
    std::shared_ptr<qlexpr::IndexMap> index_map,
    const std::shared_ptr<dockv::ReaderProjection>& unique_index_key_projection,
    const TransactionOperationContext& txn_op_context)
    : DocOperationBase(request),
      schema_version_(schema_version),
      doc_read_context_(std::move(doc_read_context)),
      index_map_holder_(std::move(index_map)),
      index_map_(*index_map_holder_),
      unique_index_key_projection_(unique_index_key_projection),
      txn_op_context_(txn_op_context)
{}

QLWriteOperation::QLWriteOperation(
    std::reference_wrapper<const QLWriteRequestMsg> request,
    SchemaVersion schema_version,
    DocReadContextPtr doc_read_context,
    std::reference_wrapper<const qlexpr::IndexMap> index_map,
    const std::shared_ptr<dockv::ReaderProjection>& unique_index_key_projection,
    const TransactionOperationContext& txn_op_context)
    : DocOperationBase(request),
      schema_version_(schema_version),
      doc_read_context_(std::move(doc_read_context)),
      index_map_(index_map),
      unique_index_key_projection_(unique_index_key_projection),
      txn_op_context_(txn_op_context)
{}

QLWriteOperation::~QLWriteOperation() = default;

Status QLWriteOperation::Init(QLResponseMsg* response) {
  response_ = response;
  insert_into_unique_index_ = request_.type() == QLWriteRequestPB::QL_STMT_INSERT &&
                              unique_index_key_projection_ != nullptr;
  require_read_ = RequireRead(request_, doc_read_context_->schema()) || insert_into_unique_index_
                  || !index_map_.empty();
  update_indexes_ = !request_.update_index_ids().empty();

  // Determine if static / non-static columns are being written.
  bool write_static_columns = false;
  bool write_non_static_columns = false;
  // TODO(Amit): Remove the DVLOGS after backfill features stabilize.
  DVLOG(4) << "Processing request " << AsString(request_);
  for (const auto& column : request_.column_values()) {
    DVLOG(4) << "Looking at column : " << AsString(column);
    auto schema_column = doc_read_context_->schema().column_by_id(ColumnId(column.column_id()));
    DVLOG(4) << "schema column : " << AsString(schema_column);
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

  bool is_range_operation = IsRangeOperation(request_, doc_read_context_->schema());

  // We need the hashed key if writing to the static columns, and need primary key if writing to
  // non-static columns or writing the full primary key (i.e. range columns are present or table
  // does not have range columns).
  return InitializeKeys(
      write_static_columns || is_range_operation,
      write_non_static_columns || !request_.range_column_values().empty() ||
      doc_read_context_->schema().num_range_key_columns() == 0);
}

void QLWriteOperation::ClearResponse() {
  if (response_) {
    response_->Clear();
  }
}

Status QLWriteOperation::InitializeKeys(const bool hashed_key, const bool primary_key) {
  // Populate the hashed and range components in the same order as they are in the table schema.
  const auto& hashed_column_values = request_.hashed_column_values();
  const auto& range_column_values = request_.range_column_values();
  auto hashed_components = VERIFY_RESULT(dockv::QLKeyColumnValuesToPrimitiveValues(
      hashed_column_values, doc_read_context_->schema(), 0,
      doc_read_context_->schema().num_hash_key_columns()));
  auto range_components = VERIFY_RESULT(dockv::QLKeyColumnValuesToPrimitiveValues(
      range_column_values, doc_read_context_->schema(),
      doc_read_context_->schema().num_hash_key_columns(),
      doc_read_context_->schema().num_range_key_columns()));

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
      // Ignore use-after-move warning since hashed_components was only moved if need_pk = false.
      pk_doc_key_.emplace(
         request_.hash_code(), std::move(hashed_components), // NOLINT(bugprone-use-after-move)
         std::move(range_components));
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
  if (mode == GetDocPathsMode::kStrongReadIntents) {
    return Status::OK();
  }
  if (mode == GetDocPathsMode::kLock || request_.column_values().empty() || !index_map_.empty()) {
    if (encoded_hashed_doc_key_) {
      paths->push_back(encoded_hashed_doc_key_);
    }
    if (encoded_pk_doc_key_) {
      paths->push_back(encoded_pk_doc_key_);
    }
  } else {
    dockv::KeyBytes buffer;
    for (const auto& column_value : request_.column_values()) {
      ColumnId column_id(column_value.column_id());
      const auto& column = VERIFY_RESULT_REF(doc_read_context_->schema().column_by_id(column_id));

      Slice doc_key = column.is_static() ? encoded_hashed_doc_key_.as_slice()
                                         : encoded_pk_doc_key_.as_slice();
      buffer.Clear();
      buffer.AppendKeyEntryType(dockv::KeyEntryType::kColumnId);
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
                                     dockv::ReaderProjection *param_static_projection,
                                     dockv::ReaderProjection *param_non_static_projection,
                                     QLTableRow* table_row) {
  auto *static_projection = param_static_projection;
  auto *non_static_projection = param_non_static_projection;

  dockv::ReaderProjection local_static_projection;
  dockv::ReaderProjection local_non_static_projection;
  if (static_projection == nullptr) {
    static_projection = &local_static_projection;
  }
  if (non_static_projection == nullptr) {
    non_static_projection = &local_non_static_projection;
  }

  // Create projections to scan docdb.
  RETURN_NOT_OK(CreateProjections(
      doc_read_context_->schema(), request_.column_refs(), static_projection, non_static_projection,
      /* full_projection = */ nullptr,
      /* don't need range keys when only hash key was specified */
      pk_doc_key_ ? AddKeysMode::kAll : AddKeysMode::kHash));

  // Generate hashed / primary key depending on if static / non-static columns are referenced in
  // the if-condition.
  RETURN_NOT_OK(InitializeKeys(
      !request_.column_refs().static_ids().empty(), !non_static_projection->columns.empty()));

  // Scan docdb for the static and non-static columns of the row using the hashed / primary key.
  if (hashed_doc_key_) {
    DocQLScanSpec spec(doc_read_context_->schema(), *hashed_doc_key_, request_.query_id());
    auto iterator = DocRowwiseIterator(
        *static_projection, *doc_read_context_, txn_op_context_,
        data.doc_write_batch->doc_db(), data.read_operation_data,
        data.doc_write_batch->pending_op());
    RETURN_NOT_OK(iterator.Init(spec));
    RETURN_NOT_OK(iterator.FetchNext(table_row));
    data.read_restart_data->MakeAtLeast(VERIFY_RESULT(iterator.GetReadRestartData()));
  }
  if (pk_doc_key_) {
    DocQLScanSpec spec(doc_read_context_->schema(), *pk_doc_key_, request_.query_id());
    auto iterator = DocRowwiseIterator(
        *non_static_projection, *doc_read_context_, txn_op_context_,
        data.doc_write_batch->doc_db(), data.read_operation_data,
        data.doc_write_batch->pending_op());
    RETURN_NOT_OK(iterator.Init(spec));
    if (VERIFY_RESULT(iterator.FetchNext(table_row))) {
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
    data.read_restart_data->MakeAtLeast(VERIFY_RESULT(iterator.GetReadRestartData()));
  }

  return Status::OK();
}

template <class F>
Status EnumProjectedColumns(
    const dockv::ReaderProjection& static_projection,
    const dockv::ReaderProjection& non_static_projection,
    const F& f) {
  for (const auto& column : non_static_projection.key_columns()) {
    RETURN_NOT_OK(f(column));
  }
  for (const auto& column : static_projection.value_columns()) {
    RETURN_NOT_OK(f(column));
  }
  for (const auto& column : non_static_projection.value_columns()) {
    RETURN_NOT_OK(f(column));
  }
  return Status::OK();
}

Status QLWriteOperation::PopulateConditionalDmlRow(
    const DocOperationApplyData& data,
    const bool should_apply,
    const QLTableRow& table_row,
    const dockv::ReaderProjection& static_projection,
    const dockv::ReaderProjection& non_static_projection,
    std::unique_ptr<qlexpr::QLRowBlock>* rowblock) {
  // Populate the result set to return the "applied" status, and optionally the hash / primary key
  // and the present column values if the condition is not satisfied and the row does exist
  // (value_map is not empty).
  const bool return_present_values = !should_apply && !table_row.IsEmpty();
  std::vector<ColumnSchema> columns;
  columns.emplace_back(ColumnSchema("[applied]", DataType::BOOL));
  if (return_present_values) {
    RETURN_NOT_OK(EnumProjectedColumns(
        static_projection, non_static_projection,
        [&columns, &schema = doc_read_context_->schema()](const auto& column) -> Status {
      columns.push_back(VERIFY_RESULT(schema.column_by_id(column.id)));
      return Status::OK();
    }));
  }
  rowblock->reset(new qlexpr::QLRowBlock{Schema(columns)});
  QLRow& row = rowblock->get()->Extend();
  row.mutable_column(0)->set_bool_value(should_apply);
  size_t col_idx = 1;
  if (return_present_values) {
    RETURN_NOT_OK(EnumProjectedColumns(
        static_projection, non_static_projection,
        [&table_row, &row, &col_idx](const auto& column) -> Status {
      return table_row.GetValue(column.id, row.mutable_column(col_idx++));
    }));
  }

  return Status::OK();
}

Status QLWriteOperation::PopulateStatusRow(const DocOperationApplyData& data,
                                           const bool should_apply,
                                           const QLTableRow& table_row,
                                           std::unique_ptr<qlexpr::QLRowBlock>* rowblock) {
  std::vector<ColumnSchema> columns;
  columns.emplace_back(ColumnSchema("[applied]", DataType::BOOL));
  columns.emplace_back(ColumnSchema("[message]", DataType::STRING));
  columns.insert(
      columns.end(), doc_read_context_->schema().columns().begin(),
      doc_read_context_->schema().columns().end());

  *rowblock = std::make_unique<qlexpr::QLRowBlock>(Schema(columns));
  QLRow& row = rowblock->get()->Extend();
  row.mutable_column(0)->set_bool_value(should_apply);
  // No message unless there is an error (then message will be set in executor).

  // If not applied report the existing row values as for regular if clause.
  if (!should_apply) {
    for (size_t i = 0; i < doc_read_context_->schema().num_columns(); i++) {
      std::optional<std::reference_wrapper<const QLValuePB>> col_val =
          table_row.GetValue(doc_read_context_->schema().column_id(i));
      if (col_val.has_value()) {
        *(row.mutable_column(i + 2)) = *col_val;
      }
    }
  }

  return Status::OK();
}

// Check if a duplicate value is inserted into a unique index.
Result<bool> QLWriteOperation::HasDuplicateUniqueIndexValue(const DocOperationApplyData& data) {
  VLOG(3) << "Looking for collisions in\n" << DocDBDebugDumpToStr(data);
  // We need to check backwards only for backfilled entries.
  bool ret =
      VERIFY_RESULT(HasDuplicateUniqueIndexValue(data, data.read_time())) ||
      (request_.is_backfill() &&
       VERIFY_RESULT(HasDuplicateUniqueIndexValueBackward(data)));
  if (!ret) {
    VLOG(3) << "No collisions found";
  }
  return ret;
}

Result<bool> QLWriteOperation::HasDuplicateUniqueIndexValueBackward(
    const DocOperationApplyData& data) {
  VLOG(2) << "Looking for collision while going backward. Trying to insert " << *pk_doc_key_;
  auto requested_read_time = data.read_time();

  char highest = dockv::KeyEntryTypeAsChar::kHighest;
  KeyBuffer upperbound_buffer(pk_doc_key_->Encode().AsSlice(), Slice(&highest, 1));
  auto iter = CreateIntentAwareIterator(
      data.doc_write_batch->doc_db(),
      BloomFilterOptions::Fixed(pk_doc_key_->Encode().AsSlice()),
      request_.query_id(),
      txn_op_context_,
      // This should be done with kMaxReadTime so that we do not filter out any records.
      data.read_operation_data.WithAlteredReadTime(ReadHybridTime::Max()));
  IntentAwareIteratorUpperboundScope upperbound_scope(upperbound_buffer.AsSlice(), iter.get());

  DocHybridTime oldest_past_min_dht = VERIFY_RESULT(FindOldestOverwrittenTimestamp(
      iter.get(), dockv::SubDocKey(*pk_doc_key_), requested_read_time.read));
  const DocHybridTime oldest_past_min_dht_liveness = VERIFY_RESULT(FindOldestOverwrittenTimestamp(
      iter.get(), dockv::SubDocKey(*pk_doc_key_, KeyEntryValue::kLivenessColumn),
      requested_read_time.read));
  // Use kMaxWriteId to include other column values that may have been written as
  // part of the write.
  const auto modified_oldest_past_min_dht_liveness =
      DocHybridTime(oldest_past_min_dht_liveness.hybrid_time(), kMaxWriteId);
  VLOG_WITH_FUNC(3) << "before MakeAtMost: oldest_past_min_dht=" << oldest_past_min_dht
                    << ", modified_oldest_past_min_dht_liveness: "
                    << modified_oldest_past_min_dht_liveness
                    << ", oldest_past_min_dht_liveness=" << oldest_past_min_dht_liveness;
  // Pick the older (smaller) DocHybridTime of the two.
  if (modified_oldest_past_min_dht_liveness.is_valid() &&
      (!oldest_past_min_dht.is_valid() ||
       modified_oldest_past_min_dht_liveness < oldest_past_min_dht)) {
    oldest_past_min_dht = modified_oldest_past_min_dht_liveness;
  }
  VLOG_WITH_FUNC(3) << "after MakeAtMost: oldest_past_min_dht=" << oldest_past_min_dht;
  if (!oldest_past_min_dht.is_valid()) {
    return false;
  }
  auto read_time = ReadHybridTime::SingleTime(oldest_past_min_dht.hybrid_time());
  return HasDuplicateUniqueIndexValue(data, read_time, oldest_past_min_dht.write_id());
}

Result<bool> QLWriteOperation::HasDuplicateUniqueIndexValue(
    const DocOperationApplyData& data, const ReadHybridTime& read_time,
    IntraTxnWriteId write_id) {
  // Set up the iterator to read the current primary key associated with the index key.
  DocQLScanSpec spec(doc_read_context_->schema(), *pk_doc_key_, request_.query_id(), true);
  auto altered_read_op_data =
      data.read_operation_data.WithAlteredReadTimeAndWriteId(read_time, write_id);
  auto iterator = DocRowwiseIterator(
      *unique_index_key_projection_,
      *doc_read_context_,
      txn_op_context_,
      data.doc_write_batch->doc_db(),
      altered_read_op_data,
      data.doc_write_batch->pending_op());
  RETURN_NOT_OK(iterator.Init(spec));

  // It is a duplicate value if the index key exists already and the index value (corresponding to
  // the indexed table's primary key) is not the same.
  QLTableRow table_row;
  if (!VERIFY_RESULT(iterator.FetchNext(&table_row))) {
    VLOG(2) << "No collision found while checking at " << yb::ToString(read_time);
    return false;
  }
  std::unordered_set<ColumnId> key_column_ids;
  for (const auto& column : unique_index_key_projection_->columns) {
    key_column_ids.insert(column.id);
  }
  for (const auto& column_value : request_.column_values()) {
    ColumnId column_id(column_value.column_id());
    if (key_column_ids.count(column_id) > 0) {
      std::optional<std::reference_wrapper<const QLValuePB>> existing_value =
          table_row.GetValue(column_id);
      const auto& new_value = column_value.expr().value();
      if (existing_value && *existing_value != new_value) {
        VLOG(2) << "Found collision while checking at " << AsString(read_time)
                << "\nExisting: " << AsString(*existing_value) << " vs New: " << AsString(new_value)
                << "\nUsed read time as " << AsString(data.read_time());
        DVLOG(3) << "DocDB is now:\n" << DocDBDebugDumpToStr(data);
        return true;
      }
    }
  }

  VLOG(2) << "No collision while checking at " << yb::ToString(read_time);
  return false;
}

Result<DocHybridTime> QLWriteOperation::FindOldestOverwrittenTimestamp(
    IntentAwareIterator* iter, const dockv::SubDocKey& sub_doc_key, HybridTime min_read_time) {
  DocHybridTime result = DocHybridTime::kInvalid;
  VLOG(3) << "Doing iter->Seek " << *pk_doc_key_;
  iter->Seek(*pk_doc_key_);
  if (VERIFY_RESULT_REF(iter->Fetch())) {
    const auto bytes = sub_doc_key.EncodeWithoutHt();
    const Slice& sub_key_slice = bytes.AsSlice();
    result = VERIFY_RESULT(
        iter->FindOldestRecord(sub_key_slice, min_read_time));
    VLOG(2) << "iter->FindOldestRecord returned " << result << " for "
            << dockv::SubDocKey::DebugSliceToString(sub_key_slice);
  } else {
    VLOG(3) << "iter->Seek " << *pk_doc_key_ << " turned out to be out of records";
  }
  return result;
}

Status QLWriteOperation::ApplyForJsonOperators(
    const ColumnSchema& column_schema,
    const ColumnId col_id,
    const JsonColumnMap& col_map,
    const ApplyContext& context,
    IsInsert is_insert,
    QLTableRow* existing_row) {
  VLOG_WITH_FUNC(3)
      << "column_schema: " << AsString(column_schema) << ", col_id: " << col_id << ", col_map: "
      << AsString(col_map) << ", is_insert: " << is_insert << ", existing row: "
      << AsString(existing_row);
  using common::Jsonb;
  rapidjson::Document document;
  QLValue qlv;
  RapidJsonMemberCache member_cache;
  bool read_needed = true;
  for (const auto& column_value_ptr : col_map.find(col_id)->second) {
    const auto& column_value = *column_value_ptr;
    if (column_value.column_id() != col_id) continue;
    if (read_needed) {
      // Read the json column value in order to perform a read modify write.
      QLExprResult temp;
      RETURN_NOT_OK(existing_row->ReadColumn(col_id, temp.Writer()));
      const auto& ql_value = temp.Value();
      if (!IsNull(ql_value)) {
        Jsonb jsonb(ql_value.jsonb_value());
        RETURN_NOT_OK(jsonb.ToRapidJson(&document));
      } else {
        if (!is_insert && column_value.json_args_size() > 1) {
          return STATUS_FORMAT(QLError, "JSON path depth should be 1 for upsert",
            column_value.ShortDebugString());
        }
        auto& column = existing_row->AllocColumn(column_value.column_id());
        column.value.set_jsonb_value(common::Jsonb::kSerializedJsonbEmpty);

        Jsonb jsonb(column.value.jsonb_value());
        RETURN_NOT_OK(jsonb.ToRapidJson(&document));
      }
      VLOG_WITH_FUNC(4) << "input document: " << common::WriteRapidJsonToString(document);
    }
    read_needed = false;

    // Deserialize the rhs.
    Jsonb rhs(column_value.expr().value().jsonb_value());
    rapidjson::Document rhs_doc(&document.GetAllocator());
    RETURN_NOT_OK(rhs.ToRapidJson(&rhs_doc));
    VLOG_WITH_FUNC(4) << "rhs document: " << common::WriteRapidJsonToString(rhs_doc);

    // Update the json value.
    rapidjson::Value::MemberIterator memberit;
    rapidjson::Value::ValueIterator valueit;
    bool last_elem_object;
    rapidjson::Value* node = &document;

    int i = 0;
    auto status = FindMemberForIndex(column_value, i, node, &memberit, &valueit,
        &last_elem_object, is_insert, &member_cache);
    auto it = column_value.json_args().begin();
    for (i = 1; ++it != column_value.json_args().end() && status.ok(); ++i) {
      node = last_elem_object ? &memberit->value : &*valueit;
      status = FindMemberForIndex(
          column_value, i, node, &memberit, &valueit, &last_elem_object, is_insert, &member_cache);
    }

    bool update_missing = false;
    if (is_insert) {
      RETURN_NOT_OK(status);
    } else {
      update_missing = !status.ok();
    }

    if (update_missing) {
      // NOTE: lhs path cannot exceed by more than one hop
      if (last_elem_object && make_unsigned(i) == column_value.json_args().size()) {
        auto prev_it = it;
        --prev_it;
        auto val = prev_it->operand().value().string_value();
        rapidjson::Value v(
            val.cdata(), narrow_cast<rapidjson::SizeType>(val.size()), document.GetAllocator());
        // Add the member and update the cache if enabled.
        VLOG_WITH_FUNC(4)
            << "add member: " << common::WriteRapidJsonToString(v) << " => "
            << common::WriteRapidJsonToString(rhs_doc);
        node->AddMember(v, rhs_doc, document.GetAllocator());
        if (FLAGS_ycql_jsonb_use_member_cache) {
          member_cache[MemberCacheValueHash(node)][std::string(val)] = node->MemberCount() - 1;
        }
      } else {
        RETURN_NOT_OK(status);
      }
    } else if (last_elem_object) {
      memberit->value = rhs_doc.Move();
    } else {
      *valueit = rhs_doc.Move();
    }
  } // end of column processing
  // Now write the new json value back.
  VLOG_WITH_FUNC(3) << "output document: " << common::WriteRapidJsonToString(document);
  Jsonb jsonb_result;
  RETURN_NOT_OK(jsonb_result.FromRapidJson(document));
  // Update the current row as well so that we can accumulate the result of multiple json
  // operations and write the final value.
  *qlv.mutable_jsonb_value() = std::move(jsonb_result.MoveSerializedJsonb());

  existing_row->AllocColumn(col_id).value = qlv.value();

  return InsertScalar(
      context, column_schema, col_id, LWQLValuePB(&response_->arena(), qlv.value()),
      bfql::TSOpcode::kScalarInsert);
}

Status QLWriteOperation::InsertScalar(
    const ApplyContext& context,
    const ColumnSchema& column_schema,
    ColumnId column_id,
    const QLValueMsg& value,
    bfql::TSOpcode op_code) {
  ValueRef value_ref(value, column_schema.sorting_type(), op_code);
  if (context.row_packer && value_ref.IsTombstoneOrPrimitive() && !column_schema.is_static() &&
      VERIFY_RESULT(context.row_packer->AddValue(
          column_id, context.column_control_fields.AsSlice(), value))) {
    return Status::OK();
  }

  return context.data->doc_write_batch->InsertSubDocument(
      MakeSubPath(column_schema, column_id), value_ref, context.data->read_operation_data,
      request_.query_id(), context.control_fields.ttl, context.control_fields.timestamp);
}

Status QLWriteOperation::ApplyForSubscriptArgs(const QLColumnValueMsg& column_value,
                                               const QLTableRow& existing_row,
                                               const ApplyContext& context,
                                               const ColumnSchema& column,
                                               ColumnId column_id) {
  qlexpr::LWExprResult expr_result(&column_value);
  RETURN_NOT_OK(EvalExpr(column_value.expr(), existing_row, expr_result.Writer()));
  ValueRef value(
      expr_result.Value(), column.sorting_type(),
      qlexpr::GetTSWriteInstruction(column_value.expr()));
  RETURN_NOT_OK(CheckUserTimestampForCollections(context.control_fields.timestamp));

  // Setting the value for a sub-column
  // Currently we only support two cases here: `map['key'] = v` and `list[index] = v`)
  // Any other case should be rejected by the semantic analyser before getting here
  // Later when we support frozen or nested collections this code may need refactoring
  DCHECK_EQ(column_value.subscript_args().size(), 1);
  DCHECK(column_value.subscript_args().front().has_value()) << "An index must be a constant";
  auto sub_path = MakeSubPath(column, column_id);
  switch (column.type()->main()) {
    case DataType::MAP: {
      sub_path.AddSubKey(KeyEntryValue::FromQLValuePB(
          column_value.subscript_args().front().value(), SortingType::kNotSpecified));
      RETURN_NOT_OK(context.data->doc_write_batch->InsertSubDocument(
          sub_path, value, context.data->read_operation_data,
          request_.query_id(), context.control_fields.ttl, context.control_fields.timestamp));
      break;
    }
    case DataType::LIST: {
      MonoDelta default_ttl = doc_read_context_->schema().table_properties().HasDefaultTimeToLive()
          ? MonoDelta::FromMilliseconds(
                doc_read_context_->schema().table_properties().DefaultTimeToLive())
          : MonoDelta::kMax;

      int target_cql_index = column_value.subscript_args().front().value().int32_value();
      RETURN_NOT_OK(context.data->doc_write_batch->ReplaceCqlInList(
          sub_path, target_cql_index, value, context.data->read_operation_data, request_.query_id(),
          default_ttl, context.control_fields.ttl));
      break;
    }
    default: {
      LOG(DFATAL) << "Unexpected type for setting subcolumn: " << column.type()->ToString();
    }
  }
  return Status::OK();
}

Status QLWriteOperation::ApplyForRegularColumns(const QLColumnValueMsg& column_value,
                                                const QLTableRow& existing_row,
                                                const ApplyContext& context,
                                                const ColumnSchema& column,
                                                ColumnId column_id,
                                                QLTableRow* new_row) {
  using bfql::TSOpcode;

  // Typical case, setting a columns value
  qlexpr::LWExprResult expr_result(&column_value);
  RETURN_NOT_OK(EvalExpr(column_value.expr(), existing_row, expr_result.Writer()));
  auto write_instruction = qlexpr::GetTSWriteInstruction(column_value.expr());
  ValueRef value(expr_result.Value(), column.sorting_type(), write_instruction);
  switch (write_instruction) {
    case TSOpcode::kToJson: FALLTHROUGH_INTENDED;
    case TSOpcode::kScalarInsert:
      RETURN_NOT_OK(InsertScalar(
          context, column, column_id, expr_result.Value(), write_instruction));
      break;
    case TSOpcode::kMapExtend:
    case TSOpcode::kSetExtend:
    case TSOpcode::kMapRemove:
    case TSOpcode::kSetRemove:
      RETURN_NOT_OK(CheckUserTimestampForCollections(context.control_fields.timestamp));
      RETURN_NOT_OK(context.data->doc_write_batch->ExtendSubDocument(
        MakeSubPath(column, column_id), value, context.data->read_operation_data,
        request_.query_id(), context.control_fields.ttl));
      break;
    case TSOpcode::kListPrepend:
      value.set_list_extend_order(dockv::ListExtendOrder::PREPEND_BLOCK);
      FALLTHROUGH_INTENDED;
    case TSOpcode::kListAppend:
      RETURN_NOT_OK(CheckUserTimestampForCollections(context.control_fields.timestamp));
      RETURN_NOT_OK(context.data->doc_write_batch->ExtendList(
          MakeSubPath(column, column_id), value, context.data->read_operation_data,
          request_.query_id(), context.control_fields.ttl));
      break;
    case TSOpcode::kListRemove:
      // TODO(akashnil or mihnea) this should call RemoveFromList once thats implemented
      // Currently list subtraction is computed in memory using builtin call so this
      // case should never be reached. Once it is implemented the corresponding case
      // from EvalQLExpressionPB should be uncommented to enable this optimization.
      RETURN_NOT_OK(CheckUserTimestampForCollections(context.control_fields.timestamp));
      RETURN_NOT_OK(context.data->doc_write_batch->InsertSubDocument(
          MakeSubPath(column, column_id), value, context.data->read_operation_data,
          request_.query_id(), context.control_fields.ttl, context.control_fields.timestamp));
      break;
    default:
      LOG(FATAL) << "Unsupported operation: " << static_cast<int>(write_instruction);
      break;
  }

  if (update_indexes_) {
    new_row->AllocColumn(column_id, expr_result.Value());
  }
  return Status::OK();
}

Status QLWriteOperation::Apply(const DocOperationApplyData& data) {
  // Anything a previous attempt accumulated is abandoned.
  index_requests_.clear();
  index_arena_.reset();

  data.doc_write_batch->SetDocReadContext(doc_read_context_);

  QLTableRow existing_row;
  if (request_.has_if_expr()) {
    // Check if the if-condition is satisfied.
    dockv::ReaderProjection static_projection, non_static_projection;
    RETURN_NOT_OK(ReadColumns(data, &static_projection, &non_static_projection, &existing_row));
    bool should_apply = VERIFY_RESULT(EvalCondition(request_.if_expr().condition(), existing_row));
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

  // Initialize the new row being written to either the existing row if read, or just populate
  // the primary key.
  QLTableRow new_row;
  if (!existing_row.IsEmpty()) {
    new_row = existing_row;
  } else {
    size_t idx = 0;
    for (const auto& expr : request_.hashed_column_values()) {
      new_row.AllocColumn(doc_read_context_->schema().column_id(idx), expr.value());
      idx++;
    }
    for (const auto& expr : request_.range_column_values()) {
      new_row.AllocColumn(doc_read_context_->schema().column_id(idx), expr.value());
      idx++;
    }
  }

  switch (request_.type()) {
    // QL insert == update (upsert) to be consistent with Cassandra's semantics. In either
    // INSERT or UPDATE, if non-key columns are specified, they will be inserted which will cause
    // the primary key to be inserted also when necessary. Otherwise, we should insert the
    // primary key at least.
    case QLWriteRequestPB::QL_STMT_INSERT:
    case QLWriteRequestPB::QL_STMT_UPDATE:
      RETURN_NOT_OK(ApplyUpsert(data, existing_row, &new_row));
      break;
    case QLWriteRequestPB::QL_STMT_DELETE:
      RETURN_NOT_OK(ApplyDelete(data, &existing_row, &new_row));
      break;
  }

  response_->set_status(QLResponsePB::YQL_STATUS_OK);

  return Status::OK();
}

UserTimeMicros QLWriteOperation::user_timestamp() const {
  return request_.has_user_timestamp_usec() ?
      request_.user_timestamp_usec() : ValueControlFields::kInvalidTimestamp;
}

DocPath QLWriteOperation::MakeSubPath(const ColumnSchema& column_schema, ColumnId column_id) {
  const auto& key = column_schema.is_static() ? encoded_hashed_doc_key_ : encoded_pk_doc_key_;
  return DocPath(key.as_slice(), KeyEntryValue::MakeColumnId(column_id));
}

Status QLWriteOperation::ApplyUpsert(
    const DocOperationApplyData& data, const QLTableRow& existing_row, QLTableRow* new_row) {
  ApplyContext context = {
    .data = &data,
    .control_fields = ValueControlFields {
      .ttl = request_ttl(),
      .timestamp = user_timestamp(),
    },
    .column_control_fields = {},
  };

  // Add the appropriate liveness column only for inserts.
  // We never use init markers for QL to ensure we perform writes without any reads to
  // ensure our write path is fast while complicating the read path a bit.
  IsInsert is_insert(request_.type() == QLWriteRequestPB::QL_STMT_INSERT);

  VLOG_WITH_FUNC(4)
      << "existing_row: " << AsString(existing_row) << ", new_row: " << AsString(new_row)
      << ", is_insert: " << is_insert;

  std::optional<dockv::RowPackerV1> row_packer;
  std::optional<IntraTxnWriteId> packed_row_write_id;

  auto se = ScopeExit([&packed_row_write_id, doc_write_batch = data.doc_write_batch]() {
    if (packed_row_write_id) {
      doc_write_batch->RollbackReservedWriteId(*packed_row_write_id);
    }
  });

  if (is_insert && encoded_pk_doc_key_) {
    bool pack_row = FLAGS_ycql_enable_packed_row;
    if (pack_row) {
      const auto& schema_packing = VERIFY_RESULT_REF(
          doc_read_context_->schema_packing_storage.GetPacking(schema_version_));
      // In YCQL user could specify timestamp that is used by inserted row.
      // As result there could be more recent data.
      // Disable packed row when user specified timestamp.
      pack_row = !context.control_fields.has_timestamp() &&
                 schema_packing.CouldPack(request_.column_values());
      if (pack_row) {
        row_packer.emplace(
            schema_version_, schema_packing, FLAGS_ycql_packed_row_size_limit,
            context.control_fields);
        packed_row_write_id = data.doc_write_batch->ReserveWriteId();
        context.row_packer = &row_packer.value();
        ValueControlFields column_control_fields;
        column_control_fields.ttl = context.control_fields.ttl;
        column_control_fields.AppendEncoded(&context.column_control_fields);
      }
    }

    if (!pack_row) {
      const DocPath sub_path(encoded_pk_doc_key_.as_slice(), KeyEntryValue::kLivenessColumn);
      RETURN_NOT_OK(data.doc_write_batch->SetPrimitive(
          sub_path, context.control_fields, ValueRef(dockv::ValueEntryType::kNullLow),
          data.read_operation_data, request_.query_id()));
    }
  }

  JsonColumnMap col_map;
  auto process_column = [&](const QLColumnValueMsg& column_value) -> Status {
    if (!column_value.has_column_id()) {
      return STATUS_FORMAT(InvalidArgument, "column id missing: $0", column_value);
    }
    const ColumnId column_id(column_value.column_id());
    const ColumnSchema& column_schema = VERIFY_RESULT(
        doc_read_context_->schema().column_by_id(column_id));

    QLValue expr_result;
    if (!column_value.json_args().empty()) {
      col_map[column_id].emplace_back(&column_value);
    } else if (!column_value.subscript_args().empty()) {
      RETURN_NOT_OK(ApplyForSubscriptArgs(
          column_value, existing_row, context, column_schema, column_id));
    } else {
      RETURN_NOT_OK(ApplyForRegularColumns(
          column_value, existing_row, context, column_schema, column_id, new_row));
    }

    return Status::OK();
  };

  const auto& column_values = request_.column_values();
  auto cmp_column_id = [](const QLColumnValueMsg& lhs, const QLColumnValueMsg& rhs) {
    return lhs.column_id() < rhs.column_id();
  };
  if (!row_packer || std::is_sorted(column_values.begin(), column_values.end(), cmp_column_id)) {
    for (const auto& column_value : column_values) {
      RETURN_NOT_OK(process_column(column_value));
    }
  } else {
    boost::container::small_vector<std::reference_wrapper<const QLColumnValueMsg>, 0x10> refs(
        column_values.begin(), column_values.end());
    std::sort(refs.begin(), refs.end(), cmp_column_id);
    for (const auto& column_value : refs) {
      RETURN_NOT_OK(process_column(column_value));
    }
  }

  for (const auto& [column_id, _] : col_map) {
    const ColumnSchema& column_schema = VERIFY_RESULT(
        doc_read_context_->schema().column_by_id(column_id));
    RETURN_NOT_OK(ApplyForJsonOperators(
        column_schema, column_id, col_map, context, is_insert, new_row));
  }

  if (row_packer) {
    auto encoded_value = VERIFY_RESULT(row_packer->Complete());
    RETURN_NOT_OK(data.doc_write_batch->SetPrimitive(
        DocPath(encoded_pk_doc_key_.as_slice()), context.control_fields, ValueRef(encoded_value),
        data.read_operation_data, request_.query_id(), *packed_row_write_id));
    packed_row_write_id.reset();
  }

  if (update_indexes_) {
    RETURN_NOT_OK(UpdateIndexes(existing_row, *new_row));
  }

  return Status::OK();
}

Status QLWriteOperation::ApplyDelete(
    const DocOperationApplyData& data, QLTableRow* existing_row, QLTableRow* new_row) {
  // We have three cases:
  // 1. If non-key columns are specified, we delete only those columns/subscripted columns.
  // 2. Otherwise, if range cols are missing, this must be a range delete.
  // 3. Otherwise, this is a normal delete.
  // Analyzer ensures these are the only cases before getting here (e.g. range deletes cannot
  // specify non-key columns).
  if (request_.column_values_size() > 0) {
    // Delete the referenced columns only.
    for (const auto& column_value : request_.column_values()) {
      CHECK(column_value.has_column_id()) << "column id missing: " << AsString(column_value);
      const ColumnId column_id(column_value.column_id());
      const auto& column = VERIFY_RESULT_REF(doc_read_context_->schema().column_by_id(column_id));

      if (!column_value.subscript_args().empty()) {
        RETURN_NOT_OK(DeleteSubscriptedColumnElement(data, column, column_value, column_id));
      } else {
        const DocPath sub_path(
            column.is_static() ?
              encoded_hashed_doc_key_.as_slice() : encoded_pk_doc_key_.as_slice(),
            KeyEntryValue::MakeColumnId(column_id));
        RETURN_NOT_OK(data.doc_write_batch->DeleteSubDoc(sub_path,
            data.read_operation_data, request_.query_id(), user_timestamp()));
        if (update_indexes_) {
          new_row->MarkTombstoned(column_id);
        }
      }
    }
    if (update_indexes_) {
      RETURN_NOT_OK(UpdateIndexes(*existing_row, *new_row));
    }
  } else if (IsRangeOperation(request_, doc_read_context_->schema())) {
    // If the range columns are not specified, we read everything and delete all rows for
    // which the where condition matches.

    // Create the schema projection -- range deletes cannot reference non-primary key columns,
    // so the non-static projection is all we need, it should contain all referenced columns.
    dockv::ReaderProjection static_projection;
    dockv::ReaderProjection projection;
    RETURN_NOT_OK(CreateProjections(
        doc_read_context_->schema(), request_.column_refs(), &static_projection, &projection,
        nullptr, AddKeysMode::kAll));

    // Construct the scan spec basing on the WHERE condition.
    auto arena = SharedSmallArena();
    auto hashed_components = VERIFY_RESULT(dockv::QLKeyColumnValuesToPrimitiveValues(
        request_.hashed_column_values(), doc_read_context_->schema(), 0,
        doc_read_context_->schema().num_hash_key_columns(), *arena));

    std::optional<int32_t> hash_code =
        request_.has_hash_code() ? std::make_optional<int32_t>(request_.hash_code()) : std::nullopt;
    const auto range_covers_whole_partition_key = !request_.has_where_expr();
    const auto include_static_columns_in_scan =
        range_covers_whole_partition_key && doc_read_context_->schema().has_statics();
    DocQLScanSpec spec(
        doc_read_context_->schema(), hash_code, /* max_hash_code= */ hash_code, arena,
        hashed_components,
        QLConditionPBPtr(request_.has_where_expr() ? &request_.where_expr().condition() : nullptr),
        nullptr, request_.query_id(), true /* is_forward_scan */,
        include_static_columns_in_scan);

    // Create iterator.
    auto iterator = DocRowwiseIterator(
        projection, *doc_read_context_, txn_op_context_, data.doc_write_batch->doc_db(),
        data.read_operation_data, data.doc_write_batch->pending_op());
    RETURN_NOT_OK(iterator.Init(spec));

    // Iterate through rows and delete those that match the condition.
    // TODO(mihnea): We do not lock here, so other write transactions coming in might appear
    // partially applied if they happen in the middle of a ranged delete.
    while (VERIFY_RESULT(iterator.FetchNext(existing_row))) {
      // Match the row with the where condition before deleting it.
      bool match = false;
      RETURN_NOT_OK(spec.Match(*existing_row, &match));
      if (match) {
        const DocPath row_path(iterator.row_key());
        RETURN_NOT_OK(DeleteRow(row_path, data.doc_write_batch, data.read_operation_data));
        if (update_indexes_) {
          liveness_column_exists_ = iterator.LivenessColumnExists();
          RETURN_NOT_OK(UpdateIndexes(*existing_row, *new_row));
        }
      }
    }
    data.read_restart_data->MakeAtLeast(VERIFY_RESULT(iterator.GetReadRestartData()));
  } else {
    // Otherwise, delete the referenced row (all columns).
    RETURN_NOT_OK(DeleteRow(DocPath(encoded_pk_doc_key_.as_slice()), data.doc_write_batch,
                            data.read_operation_data));
    if (update_indexes_) {
      RETURN_NOT_OK(UpdateIndexes(*existing_row, *new_row));
    }
  }

  return Status::OK();
}

Status QLWriteOperation::DeleteSubscriptedColumnElement(
    const DocOperationApplyData& data, const ColumnSchema& column_schema,
    const QLColumnValueMsg& column_value, ColumnId column_id) {
  RETURN_NOT_OK(CheckUserTimestampForCollections(user_timestamp()));

  // Currently we only support two cases here: `DELETE map['key'] ..` and `DELETE list[index] ..`)
  // Any other case should be rejected by the semantic analyser before getting here.
  LOG_IF(DFATAL, column_value.subscript_args().size() != 1) << "Expected only one subscripted arg";
  LOG_IF(DFATAL, !column_value.subscript_args().front().has_value())
      << "An index must be a constant";
  auto sub_path = MakeSubPath(column_schema, column_id);
  switch (column_schema.type()->main()) {
    case DataType::MAP: {
      sub_path.AddSubKey(KeyEntryValue::FromQLValuePB(
          column_value.subscript_args().front().value(), SortingType::kNotSpecified));
      RETURN_NOT_OK(data.doc_write_batch->DeleteSubDoc(
          sub_path, data.read_operation_data, request_.query_id(), user_timestamp()));
      break;
    }
    case DataType::LIST: {
      const MonoDelta default_ttl =
          doc_read_context_->schema().table_properties().HasDefaultTimeToLive()
              ? MonoDelta::FromMilliseconds(
                    doc_read_context_->schema().table_properties().DefaultTimeToLive())
              : MonoDelta::kMax;

      const int target_cql_index = column_value.subscript_args().front().value().int32_value();
      // Replace value at target_cql_index with a tombstone.
      RETURN_NOT_OK(data.doc_write_batch->ReplaceCqlInList(
          sub_path, target_cql_index, ValueRef(dockv::ValueEntryType::kTombstone),
          data.read_operation_data, request_.query_id(), default_ttl, ValueControlFields::kMaxTtl));
      break;
    }
    default: {
      LOG(DFATAL) << "Unexpected type for deleting subscripted column element: "
                  << column_schema.type()->ToString();
      return STATUS_FORMAT(InternalError,
          "Unexpected type for deleting subscripted column element: $0", *column_schema.type());
    }
  }

  return Status::OK();
}

Status QLWriteOperation::DeleteRow(const DocPath& row_path, DocWriteBatch* doc_write_batch,
                                   const ReadOperationData& read_operation_data) {
  if (request_.has_user_timestamp_usec()) {
    // If user_timestamp is provided, we need to add a tombstone for each individual
    // column in the schema since we don't want to analyze this on the read path.
    for (auto i = doc_read_context_->schema().num_key_columns();
         i < doc_read_context_->schema().num_columns(); i++) {
      const DocPath sub_path(row_path.encoded_doc_key(),
                             KeyEntryValue::MakeColumnId(doc_read_context_->schema().column_id(i)));
      RETURN_NOT_OK(doc_write_batch->DeleteSubDoc(sub_path,
                                                  read_operation_data,
                                                  request_.query_id(),
                                                  request_.user_timestamp_usec()));
    }

    // Delete the liveness column as well.
    const DocPath liveness_column(row_path.encoded_doc_key(), KeyEntryValue::kLivenessColumn);
    RETURN_NOT_OK(doc_write_batch->DeleteSubDoc(liveness_column,
                                                read_operation_data,
                                                request_.query_id(),
                                                request_.user_timestamp_usec()));
  } else {
    RETURN_NOT_OK(doc_write_batch->DeleteSubDoc(row_path, read_operation_data));
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

Result<bool> QLWriteOperation::IsRowDeleted(const QLTableRow& existing_row,
                                            const QLTableRow& new_row) const {
  // Delete the whole row?
  if (request_.type() == QLWriteRequestPB::QL_STMT_DELETE && request_.column_values().empty()) {
    return true;
  }

  if (existing_row.IsEmpty()) { // If the row doesn't exist, don't check further.
    return false;
  }

  // For update/delete, if there is no liveness column, the row will be deleted after the DML unless
  // a non-null column still remains.
  if ((request_.type() == QLWriteRequestPB::QL_STMT_UPDATE ||
       request_.type() == QLWriteRequestPB::QL_STMT_DELETE) &&
      !liveness_column_exists_) {
    for (size_t idx = doc_read_context_->schema().num_key_columns();
         idx < doc_read_context_->schema().num_columns(); idx++) {
      if (doc_read_context_->schema().column(idx).is_static()) {
        continue;
      }
      const ColumnId column_id = doc_read_context_->schema().column_id(idx);
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

    #if DCHECK_IS_ON()
    // If (for all non_pk cols new_row has value NULL/kMissing i.e., the UPDATE statement only sets
    //     some/all cols to NULL)
    // then (existing_row should have a value read from docdb for all non_pk
    //       cols that are kMissing in new_row so that we can decide if the row is deleted or not).

    bool skip_check = false;
    for (size_t idx = doc_read_context_->schema().num_key_columns();
         idx < doc_read_context_->schema().num_columns(); idx++) {
      const ColumnId column_id = doc_read_context_->schema().column_id(idx);
      if (GetValueState(new_row, column_id) == ValueState::kNotNull) skip_check = true;
    }

    if (!skip_check) {
      for (size_t idx = doc_read_context_->schema().num_key_columns();
           idx < doc_read_context_->schema().num_columns(); idx++) {
        const ColumnId column_id = doc_read_context_->schema().column_id(idx);
        if (GetValueState(new_row, column_id) == ValueState::kMissing) {
          DCHECK(GetValueState(existing_row, column_id) != ValueState::kMissing);
        }
      }
    }
    #endif

    return true;
  }

  return false;
}

MonoDelta QLWriteOperation::request_ttl() const {
  return request_.has_ttl() ? MonoDelta::FromMilliseconds(request_.ttl())
                            : ValueControlFields::kMaxTtl;
}

namespace {

QLExpressionMsg* NewKeyColumn(
    QLWriteRequestMsg* request, const qlexpr::IndexInfo& index, const size_t idx) {
  return idx < index.hash_column_count()
          ? request->add_hashed_column_values()
          : request->add_range_column_values();
}

QLWriteRequestMsg* NewIndexRequest(
    const qlexpr::IndexInfo& index,
    ThreadSafeArena& arena,
    QLWriteRequestPB::QLStmtType type,
    IndexRequests* index_requests) {
  index_requests->emplace_back(&index, arena.ArenaObjectFactory());
  auto* request = index_requests->back().second;
  request->set_type(type);
  return request;
}

} // namespace

Status QLWriteOperation::UpdateIndexes(const QLTableRow& existing_row, const QLTableRow& new_row) {
  // Prepare the write requests to update the indexes. There should be at most 2 requests for each
  // index (one insert and one delete).
  VLOG(2) << "Updating indexes, existing: " << existing_row.ToString() << ", new: "
          << new_row.ToString();
  const auto& index_ids = request_.update_index_ids();
  // index_requests_ holds raw pointers into index_arena_, so the arena has to outlive every call
  // made within one apply attempt. Apply() resets both when a new attempt starts.
  if (!index_arena_) {
    index_arena_ = SharedThreadSafeArena();
    index_requests_.reserve(index_ids.size() * 2);
  }
  for (const auto& index_id : index_ids) {
    const auto* index = VERIFY_RESULT(index_map_.FindIndex(index_id));
    bool index_key_changed = false;
    bool index_pred_new_row = true;
    bool is_row_deleted = VERIFY_RESULT(IsRowDeleted(existing_row, new_row));

    bool index_pred_existing_row =
        !index->where_predicate_spec() ||
        VERIFY_RESULT(EvalCondition(
            index->where_predicate_spec()->where_expr().condition(), existing_row));

    if (is_row_deleted) {
      // If it is a partial index and predicate wasn't satisfied for the existing row
      // which is being deleted, we need to do nothing.
      if (index->where_predicate_spec() && !index_pred_existing_row) {
        VLOG(3) << "Skip index entry delete for index_id=" << index->table_id() <<
          " since predicate not satisfied";
        continue;
      }
      index_key_changed = true;
    } else {
      VERIFY_RESULT(CreateAndSetupIndexInsertRequest(
          this, index->HasWritePermission(), existing_row, new_row, index, *index_arena_,
          &index_requests_, &index_key_changed, &index_pred_new_row, index_pred_existing_row));
    }

    bool index_pred_switched_to_false = false;
    if (index->where_predicate_spec() &&
        !existing_row.IsEmpty() && index_pred_existing_row && !index_pred_new_row)
      index_pred_switched_to_false = true;

    // If the index key is changed, delete the current key.
    if ((index_key_changed || index_pred_switched_to_false) && index->HasDeletePermission()) {
      if (!index_pred_switched_to_false) {
        // 1. In case of a switch of predicate satisfiability to false, we surely have to delete the
        // row. (Even if there wasn't any index key change).
        // 2. But in case of an index key change without predicate satisfiability switch, if the
        // index predicate was already false for the existing row, we have to do nothing.
        // TODO(Piyush): Ensure EvalCondition returns an error if some column is missing.
        if (!index_pred_existing_row) {
          VLOG(3) << "Skip index entry delete of existing row for index_id=" << index->table_id() <<
            " since predicate not satisfied";
          continue;
        }
      }

      // The index PK cannot be calculated if the table row has already been deleted.
      if (existing_row.IsEmpty()) {
        VLOG(3) << "Skip index entry delete of existing row for index_id=" << index->table_id() <<
          " since existing row is not available";
        continue;
      }

      auto* const index_request = NewIndexRequest(
          *index, *index_arena_, QLWriteRequestPB::QL_STMT_DELETE, &index_requests_);
      VLOG(3) << "Issue index entry delete of existing row for index_id=" << index->table_id() <<
        " since predicate was satisfied earlier AND (isn't satisfied now (OR) the key has changed)";

      for (size_t idx = 0; idx < index->key_column_count(); idx++) {
        const auto& index_column = index->column(idx);
        auto *key_column = NewKeyColumn(index_request, *index, idx);

        // For old message expr_case() == NOT SET.
        // For new message expr_case == kColumnId when indexing expression is a column-ref.
        if (index_column.colexpr->expr_case() != QLExpressionPB::ExprCase::EXPR_NOT_SET &&
            index_column.colexpr->expr_case() != QLExpressionPB::ExprCase::kColumnId) {
          qlexpr::LWExprResult result(index_arena_.get());
          auto old_pos = index_column.colexpr->arena().AllocateBytes(0);
          RETURN_NOT_OK(EvalExpr(*index_column.colexpr, existing_row, result.Writer()));
          auto new_pos = index_column.colexpr->arena().AllocateBytes(0);
          DCHECK_EQ(old_pos, new_pos);
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

Result<QLWriteRequestMsg*> CreateAndSetupIndexInsertRequest(
    qlexpr::QLExprExecutor* expr_executor,
    bool index_has_write_permission,
    const QLTableRow& existing_row,
    const QLTableRow& new_row,
    const qlexpr::IndexInfo* index,
    ThreadSafeArena& arena,
    IndexRequests* index_requests,
    bool* has_index_key_changed,
    bool* index_pred_new_row,
    bool index_pred_existing_row) {
  bool index_key_changed = false;
  bool update_this_index = false;
  std::vector<LWQLValuePB*> values(index->columns().size(), nullptr);

  // Prepare the new index key.
  for (size_t idx = 0; idx < index->key_column_count(); idx++) {
    const auto& index_column = index->column(idx);
    bool column_changed = true;

    // Column_id should be used without executing "colexpr" for the following cases (we want
    // to avoid executing colexpr as it is less efficient).
    // - Old PROTO messages (expr_case() == NOT SET).
    // - When indexing expression is just a column-ref (expr_case == kColumnId)
    if (index_column.colexpr->expr_case() == QLExpressionPB::ExprCase::EXPR_NOT_SET ||
        index_column.colexpr->expr_case() == QLExpressionPB::ExprCase::kColumnId) {
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
          // TODO(Piyush): This else is possibly dead code. It can never happen that the new_row
          // doesn't have some column but the existing one does because we copy the existing one
          // into the new one before this function call.
          result = existing_row.GetValue(index_column.indexed_column_id);
        }
      }
      if (result) {
        values[idx] = arena.NewArenaObject<LWQLValuePB>(*result);
      }
    } else {
      qlexpr::LWExprResult result(&arena);
      if (existing_row.IsEmpty()) {
        RETURN_NOT_OK(expr_executor->EvalExpr(*index_column.colexpr, new_row, result.Writer()));
      } else {
        // For each column in the index key, if there is a new value, see if the value is
        // specified in the new value. Otherwise, use the current value.
        if (new_row.IsColumnSpecified(index_column.indexed_column_id)) {
          RETURN_NOT_OK(expr_executor->EvalExpr(*index_column.colexpr, new_row, result.Writer()));
          // Compare new and existing results of the expression, if the results are equal
          // that means the key is NOT changed in fact even if the column value is changed.
          qlexpr::LWExprResult existing_result(&arena);
          RETURN_NOT_OK(expr_executor->EvalExpr(
              *index_column.colexpr, existing_row, existing_result.Writer()));
          if (result.Value() == existing_result.Value()) {
            column_changed = false;
          } else {
            index_key_changed = true;
          }
        } else {
          // TODO(Piyush): This else is possibly dead code.
          RETURN_NOT_OK(expr_executor->EvalExpr(
              *index_column.colexpr, existing_row, result.Writer()));
        }
      }

      values[idx] = const_cast<LWQLValuePB*>(&result.Value());
    }

    if (column_changed) {
      update_this_index = true;
    }
  }

  // Prepare the covering columns.
  for (size_t idx = index->key_column_count(); idx < index->columns().size(); idx++) {
    const auto& index_column = index->column(idx);
    auto result = new_row.GetValue(index_column.indexed_column_id);
    bool column_changed = true;

    // If the index value is changed and there is no new covering column value set, use the
    // current value.
    if (index_key_changed) {
      if (!result) {
        // TODO(Piyush): This if is possibly dead code.
        result = existing_row.GetValue(index_column.indexed_column_id);
      }
    } else if (!FLAGS_ycql_disable_index_updating_optimization &&
        result && new_row.MatchColumn(index_column.indexed_column_id, existing_row)) {
      column_changed = false;
    }
    if (result) {
      values[idx] = arena.NewArenaObject<LWQLValuePB>(*result);
    }

    if (column_changed) {
      update_this_index = true;
    }
  }

  if (has_index_key_changed) {
    *has_index_key_changed = index_key_changed;
  }

  bool new_row_satisfies_idx_pred = true;
  if (index->where_predicate_spec()) {
    // TODO(Piyush): Ensure EvalCondition returns an error if some column is missing.
    new_row_satisfies_idx_pred = VERIFY_RESULT(expr_executor->EvalCondition(
        index->where_predicate_spec()->where_expr().condition(), new_row));
    VLOG(2) << "Eval condition on partial index, new: " << new_row_satisfies_idx_pred
            << ", existing: " << index_pred_existing_row;
    if (index_pred_new_row) {
      *index_pred_new_row = new_row_satisfies_idx_pred;
    }

    if (new_row_satisfies_idx_pred && !index_pred_existing_row) {
      // In case the row is unchanged but the predicate switches to true (can happen if the
      // predicate involves no indexed/covering cols).
      VLOG_IF(3, !update_this_index)
          << "Indexed/covering cols unchanged but predicate switched to true for index_id="
          << index->table_id();
      update_this_index = true;
    }
  } else {
    VLOG(3) << "No where predicate for index " << index->table_id();
  }

  if (index_has_write_permission &&
      (update_this_index || FLAGS_ycql_disable_index_updating_optimization)) {
    // If this is a partial index and the index predicate is false for the new row, skip the insert.
    if (index->where_predicate_spec() && !new_row_satisfies_idx_pred) {
      VLOG(3) << "Skip index entry write for index_id=" << index->table_id() <<
        " since predicate not satisfied";
      return nullptr;
    }

    auto* const index_request =
        NewIndexRequest(*index, arena, QLWriteRequestPB::QL_STMT_INSERT, index_requests);

    // Setup the key columns.
    for (size_t idx = 0; idx < index->key_column_count(); idx++) {
      auto* const key_column = NewKeyColumn(index_request, *index, idx);
      if (values[idx]) {
        key_column->ref_value(values[idx]);
      }
    }

    // Setup the covering columns.
    for (size_t idx = index->key_column_count(); idx < index->columns().size(); idx++) {
      if (values[idx]) {
        const auto& index_column = index->column(idx);
        auto* const covering_column = index_request->add_column_values();
        covering_column->set_column_id(index_column.column_id);
        covering_column->mutable_expr()->ref_value(values[idx]);
      }
    }

    return index_request;
  }

  return nullptr; // The index updating was skipped.
}

YB_DEFINE_ENUM(NextRowState, (kNone)(kFound)(kNotFound));

Status QLReadOperation::Execute(const YQLStorageIf& ql_storage,
                                const ReadOperationData& read_operation_data,
                                const DocReadContext& doc_read_context,
                                std::reference_wrapper<const ScopedRWOperation> pending_op,
                                QLResultSet* resultset,
                                ReadRestartData* read_restart_data) {
  auto se = ScopeExit([resultset] {
    resultset->Complete();
  });

  const auto& schema = doc_read_context.schema();
  SimulateTimeoutIfTesting(const_cast<CoarseTimePoint*>(&read_operation_data.deadline));
  size_t row_count_limit = std::numeric_limits<std::size_t>::max();
  size_t num_rows_skipped = 0;
  size_t offset = 0;

  // Read RPC for aggregates can't have a limit.
  LOG_IF(DFATAL, request_.is_aggregate() && request_.has_limit()) << "QLRead request "
      "for aggregates should not specify a limit: " << request_.ShortDebugString();

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
  dockv::ReaderProjection static_projection, non_static_projection, full_projection;
  RETURN_NOT_OK(CreateProjections(
      schema, request_.column_refs(), &static_projection, &non_static_projection,
      &full_projection, AddKeysMode::kNone));
  const bool read_static_columns = !static_projection.columns.empty();
  const bool read_distinct_columns = request_.distinct();

  VLOG_WITH_FUNC(4) << "column_refs: " << AsString(request_.column_refs());

  std::unique_ptr<YQLRowwiseIteratorIf> iter;
  std::unique_ptr<qlexpr::QLScanSpec> spec, static_row_spec;
  RETURN_NOT_OK(ql_storage.BuildYQLScanSpec(
      request_, read_operation_data.read_time, schema, read_static_columns, &spec,
      &static_row_spec));
  RETURN_NOT_OK(ql_storage.GetIterator(
      request_, full_projection, doc_read_context, txn_op_context_, read_operation_data,
      *spec, pending_op, &iter));
  VTRACE(1, "Initialized iterator");

  QLTableRow static_row;
  QLTableRow next_static_row;
  QLTableRow non_static_row;
  QLTableRow& selected_row = read_distinct_columns ? static_row : non_static_row;

  // In case when we are continuing a select with a paging state, or when using a reverse scan,
  // the static columns for the next row to fetch are not included in the first iterator and we
  // need to fetch them with a separate spec and iterator before beginning the normal fetch below.
  if (static_row_spec != nullptr) {
    std::unique_ptr<YQLRowwiseIteratorIf> static_row_iter;
    RETURN_NOT_OK(ql_storage.GetIterator(
        request_, static_projection, doc_read_context, txn_op_context_, read_operation_data,
        *static_row_spec, pending_op, &static_row_iter));
    RETURN_NOT_OK(static_row_iter->FetchNext(&static_row));
  }

  // Begin the normal fetch.
  int match_count = 0;
  bool static_dealt_with = true;
  NextRowState next_row_state = NextRowState::kNone;
  while (resultset->rsrow_count() < row_count_limit) {
    if (next_row_state == NextRowState::kNone) {
      if (!VERIFY_RESULT(iter->FetchNext(
             &non_static_row, &non_static_projection, &static_row, &static_projection))) {
        break;
      }
    } else if (next_row_state == NextRowState::kNotFound) {
      break;
    } else {
      next_row_state = NextRowState::kNone;
    }
    const bool last_read_static = iter->IsFetchedRowStatic();

    // Note that static columns are sorted before non-static columns in DocDB as follows. This is
    // because "<empty_range_components>" is empty and terminated by kGroupEnd which sorts before
    // all other ValueType characters in a non-empty range component.
    //   <hash_code><hash_components><empty_range_components><static_column_id> -> value;
    //   <hash_code><hash_components><range_components><non_static_column_id> -> value;

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
        next_row_state = VERIFY_RESULT(iter->FetchNext(
                &non_static_row, &non_static_projection, &next_static_row, &static_projection))
            ? NextRowState::kFound : NextRowState::kNotFound;
        if (next_row_state == NextRowState::kFound && !iter->IsFetchedRowStatic()) {
          static_dealt_with = false;
          continue;
        }
        AddProjection(non_static_projection, &static_row);
        RETURN_NOT_OK(AddRowToResult(spec, static_row, row_count_limit, offset, resultset,
                                     &match_count, &num_rows_skipped));
        if (next_row_state == NextRowState::kFound) {
          static_row = next_static_row;
        }
      } else {
        // We also have to do the join if we are not reading any static columns, as Cassandra
        // reports nulls for static rows with no corresponding non-static row
        if (read_static_columns || !static_dealt_with) {
          const bool join_successful = JoinStaticRow(
              schema, static_projection, static_row, &non_static_row);
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

  VLOG_WITH_FUNC(3) << "Fetched rows: " << resultset->rsrow_count() << " from "
                    << AsString(ql_storage);
  VTRACE(1, "Fetched $0 rows.", resultset->rsrow_count());

  RETURN_NOT_OK(SetPagingStateIfNecessary(
      iter.get(), resultset, row_count_limit, num_rows_skipped, read_operation_data.read_time));

  // SetPagingStateIfNecessary could perform read, so we assign read_restart_data after it.
  *read_restart_data = VERIFY_RESULT(iter->GetReadRestartData());

  return Status::OK();
}

Status QLReadOperation::SetPagingStateIfNecessary(YQLRowwiseIteratorIf* iter,
                                                  const QLResultSet* resultset,
                                                  const size_t row_count_limit,
                                                  const size_t num_rows_skipped,
                                                  const ReadHybridTime& read_time) {
  if ((resultset->rsrow_count() >= row_count_limit || request_.has_offset()) &&
      !request_.is_aggregate()) {
    dockv::SubDocKey next_row_key = VERIFY_RESULT(iter->GetSubDocKey());
    // When the "limit" number of rows are returned and we are asked to return the paging state,
    // return the partition key and row key of the next row to read in the paging state if there are
    // still more rows to read. Otherwise, leave the paging state empty which means we are done
    // reading from this tablet.
    if (request_.return_paging_state()) {
      if (!next_row_key.doc_key().empty()) {
        auto* paging_state = response_->mutable_paging_state();
        paging_state->dup_next_partition_key(
            dockv::PartitionSchema::EncodeMultiColumnHashValue(next_row_key.doc_key().hash()));
        paging_state->dup_next_row_key(next_row_key.Encode().ToStringBuffer());
        paging_state->set_total_rows_skipped(request_.paging_state().total_rows_skipped() +
            num_rows_skipped);
      } else if (request_.has_offset()) {
        auto* paging_state = response_->mutable_paging_state();
        paging_state->set_total_rows_skipped(request_.paging_state().total_rows_skipped() +
            num_rows_skipped);
      }
    }
    if (response_->has_paging_state()) {
      if (FLAGS_ycql_consistent_transactional_paging) {
        read_time.AddToPB(response_->mutable_paging_state());
      } else {
        // Using SingleTime will help avoid read restarts on second page and later but will
        // potentially produce stale results on those pages.
        auto per_row_consistent_read_time = ReadHybridTime::SingleTime(read_time.read);
        per_row_consistent_read_time.AddToPB(response_->mutable_paging_state());
      }
    }
  }

  return Status::OK();
}

Status QLReadOperation::GetIntents(const Schema& schema, LWKeyValueWriteBatchPB* out) {
  auto hashed_components = VERIFY_RESULT(dockv::QLKeyColumnValuesToPrimitiveValues(
      request_.hashed_column_values(), schema, 0, schema.num_hash_key_columns()));
  auto* pair = out->add_read_pairs();
  if (hashed_components.empty()) {
    // Empty hashed components mean that we don't have primary key at all, but request
    // could still contain hash_code as part of tablet routing.
    // So we should ignore it.
    pair->dup_key(std::string(1, dockv::KeyEntryTypeAsChar::kGroupEnd));
  } else {
    dockv::DocKey doc_key(request_.hash_code(), hashed_components);
    pair->dup_key(doc_key.Encode().AsSlice());
  }
  pair->dup_value(std::string(1, dockv::ValueEntryTypeAsChar::kNullLow));
  // Wait policies make sense only for YSQL to support different modes like waiting, skipping, or
  // failing on detecting intent conflicts. YCQL behaviour matches Fail-on-Conflict always (see
  // proto for details).
  out->set_wait_policy(WAIT_ERROR);
  return Status::OK();
}

Status QLReadOperation::PopulateResultSet(const std::unique_ptr<qlexpr::QLScanSpec>& spec,
                                          const QLTableRow& table_row,
                                          QLResultSet *resultset) {
  resultset->AllocateRow();
  int rscol_index = 0;
  for (const auto& expr : request_.selected_exprs()) {
    qlexpr::LWExprResult value(&request_.arena());
    RETURN_NOT_OK(EvalExpr(expr, table_row, value.Writer(), &spec->schema()));
    resultset->AppendColumn(rscol_index, value.Value());
    rscol_index++;
  }

  return Status::OK();
}

Status QLReadOperation::EvalAggregate(const QLTableRow& table_row) {
  PrepareAggregateResults(request_.selected_exprs().size());

  int aggr_index = 0;
  for (const auto& expr : request_.selected_exprs()) {
    RETURN_NOT_OK(EvalExpr(expr, table_row, aggr_result_[aggr_index++].Writer()));
  }
  return Status::OK();
}

Status QLReadOperation::PopulateAggregate(const QLTableRow& table_row, QLResultSet *resultset) {
  resultset->AllocateRow();
  for (auto rscol_index : Range(request_.selected_exprs().size())) {
    resultset->AppendColumn(rscol_index, aggr_result_[rscol_index].Value());
  }
  return Status::OK();
}

Status QLReadOperation::AddRowToResult(const std::unique_ptr<qlexpr::QLScanSpec>& spec,
                                       const QLTableRow& row,
                                       const size_t row_count_limit,
                                       const size_t offset,
                                       QLResultSet* resultset,
                                       int* match_count,
                                       size_t *num_rows_skipped) {
  VLOG_WITH_FUNC(4) << AsString(row);
  if (resultset->rsrow_count() >= row_count_limit) {
    return Status::OK();
  }
  bool match = false;
  RETURN_NOT_OK(spec->Match(row, &match));
  VLOG_WITH_FUNC(4) << "match: " << match << ", num_rows_skipped: " << *num_rows_skipped
                    << ", offset: " << offset;
  if (!match) {
    return Status::OK();
  }
  if (*num_rows_skipped >= offset) {
    ++*match_count;
    if (request_.is_aggregate()) {
      return EvalAggregate(row);
    }

    return PopulateResultSet(spec, row, resultset);
  }

  ++*num_rows_skipped;
  return Status::OK();
}

}  // namespace docdb
}  // namespace yb
