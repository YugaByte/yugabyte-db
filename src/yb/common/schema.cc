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

#include "yb/common/schema.h"

#include <set>
#include <algorithm>

#include "yb/gutil/stringprintf.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/strcat.h"
#include "yb/util/malloc.h"
#include "yb/util/status.h"
#include "yb/common/row.h"

namespace yb {

using std::shared_ptr;
using std::set;
using std::unordered_map;
using std::unordered_set;

// ------------------------------------------------------------------------------------------------
// ColumnSchema
// ------------------------------------------------------------------------------------------------

// TODO: include attributes_.ToString() -- need to fix unit tests
// first
string ColumnSchema::ToString() const {
  return strings::Substitute("$0[$1]",
                             name_,
                             TypeToString());
}

string ColumnSchema::TypeToString() const {
  return strings::Substitute("$0 $1 $2",
                             type_info()->name(),
                             is_nullable_ ? "NULLABLE" : "NOT NULL",
                             is_hash_key_ ? "PARTITION KEY" : "NOT A PARTITION KEY");
}

size_t ColumnSchema::memory_footprint_excluding_this() const {
  // Rough approximation.
  return name_.capacity();
}

size_t ColumnSchema::memory_footprint_including_this() const {
  return malloc_usable_size(this) + memory_footprint_excluding_this();
}

// ------------------------------------------------------------------------------------------------
// TableProperties
// ------------------------------------------------------------------------------------------------

void TableProperties::ToTablePropertiesPB(TablePropertiesPB *pb) const {
  if (HasDefaultTimeToLive()) {
    pb->set_default_time_to_live(default_time_to_live_);
  }
  pb->set_contain_counters(contain_counters_);
  pb->set_is_transactional(is_transactional_);
  pb->set_consistency_level(consistency_level_);
  if (HasCopartitionTableId()) {
    pb->set_copartition_table_id(copartition_table_id_);
  }
  pb->set_use_mangled_column_name(use_mangled_column_name_);
  if (HasNumTablets()) {
    pb->set_num_tablets(num_tablets_);
  }
  pb->set_is_ysql_catalog_table(is_ysql_catalog_table_);
  pb->set_is_backfilling(is_backfilling_);
}

TableProperties TableProperties::FromTablePropertiesPB(const TablePropertiesPB& pb) {
  TableProperties table_properties;
  if (pb.has_default_time_to_live()) {
    table_properties.SetDefaultTimeToLive(pb.default_time_to_live());
  }
  if (pb.has_contain_counters()) {
    table_properties.SetContainCounters(pb.contain_counters());
  }
  if (pb.has_is_transactional()) {
    table_properties.SetTransactional(pb.is_transactional());
  }
  if (pb.has_consistency_level()) {
    table_properties.SetConsistencyLevel(pb.consistency_level());
  }
  if (pb.has_copartition_table_id()) {
    table_properties.SetCopartitionTableId(pb.copartition_table_id());
  }
  if (pb.has_use_mangled_column_name()) {
    table_properties.SetUseMangledColumnName(pb.use_mangled_column_name());
  }
  if (pb.has_num_tablets()) {
    table_properties.SetNumTablets(pb.num_tablets());
  }
  if (pb.has_is_ysql_catalog_table()) {
    table_properties.set_is_ysql_catalog_table(pb.is_ysql_catalog_table());
  }
  if (pb.has_is_backfilling()) {
    table_properties.SetIsBackfilling(pb.is_backfilling());
  }
  return table_properties;
}

void TableProperties::AlterFromTablePropertiesPB(const TablePropertiesPB& pb) {
  if (pb.has_default_time_to_live()) {
    SetDefaultTimeToLive(pb.default_time_to_live());
  }
  if (pb.has_is_transactional()) {
    SetTransactional(pb.is_transactional());
  }
  if (pb.has_consistency_level()) {
    SetConsistencyLevel(pb.consistency_level());
  }
  if (pb.has_copartition_table_id()) {
    SetCopartitionTableId(pb.copartition_table_id());
  }
  if (pb.has_use_mangled_column_name()) {
    SetUseMangledColumnName(pb.use_mangled_column_name());
  }
  if (pb.has_num_tablets()) {
    SetNumTablets(pb.num_tablets());
  }
  if (pb.has_is_ysql_catalog_table()) {
    set_is_ysql_catalog_table(pb.is_ysql_catalog_table());
  }
  if (pb.has_is_backfilling()) {
    SetIsBackfilling(pb.is_backfilling());
  }
}

void TableProperties::Reset() {
  default_time_to_live_ = kNoDefaultTtl;
  contain_counters_ = false;
  is_transactional_ = false;
  consistency_level_ = YBConsistencyLevel::STRONG;
  copartition_table_id_ = kNoCopartitionTableId;
  use_mangled_column_name_ = false;
  num_tablets_ = 0;
  is_ysql_catalog_table_ = false;
  is_backfilling_ = false;
}

string TableProperties::ToString() const {
  std::string result("{ ");
  if (HasDefaultTimeToLive()) {
    result += Format("default_time_to_live: $0 ", default_time_to_live_);
  }
  result += Format("contain_counters: $0 is_transactional: $1 ",
                   contain_counters_, is_transactional_);
  if (HasCopartitionTableId()) {
    result += Format("copartition_table_id: $0 ", copartition_table_id_);
  }
  return result + Format(
      "consistency_level: $0 is_ysql_catalog_table: $1 }",
      consistency_level_,
      is_ysql_catalog_table_);
}

// ------------------------------------------------------------------------------------------------
// Schema
// ------------------------------------------------------------------------------------------------

Schema::Schema(const Schema& other)
  : name_to_index_bytes_(0),
    // TODO: C++11 provides a single-arg constructor
    name_to_index_(10,
                   NameToIndexMap::hasher(),
                   NameToIndexMap::key_equal(),
                   NameToIndexMapAllocator(&name_to_index_bytes_)) {
  CopyFrom(other);
}

Schema& Schema::operator=(const Schema& other) {
  if (&other != this) {
    CopyFrom(other);
  }
  return *this;
}

void Schema::CopyFrom(const Schema& other) {
  num_key_columns_ = other.num_key_columns_;
  num_hash_key_columns_ = other.num_hash_key_columns_;
  cols_ = other.cols_;
  col_ids_ = other.col_ids_;
  col_offsets_ = other.col_offsets_;
  id_to_index_ = other.id_to_index_;

  // We can't simply copy name_to_index_ since the GStringPiece keys
  // reference the other Schema's ColumnSchema objects.
  name_to_index_.clear();
  int i = 0;
  for (const ColumnSchema &col : cols_) {
    // The map uses the 'name' string from within the ColumnSchema object.
    name_to_index_[col.name()] = i++;
  }

  has_nullables_ = other.has_nullables_;
  has_statics_ = other.has_statics_;
  table_properties_ = other.table_properties_;
  cotable_id_ = other.cotable_id_;
  pgtable_id_ = other.pgtable_id_;

  // Schema cannot have both, cotable ID and pgtable ID.
  DCHECK(cotable_id_.IsNil() || pgtable_id_ == 0);
}

void Schema::swap(Schema& other) {
  std::swap(num_key_columns_, other.num_key_columns_);
  std::swap(num_hash_key_columns_, other.num_hash_key_columns_);
  cols_.swap(other.cols_);
  col_ids_.swap(other.col_ids_);
  col_offsets_.swap(other.col_offsets_);
  name_to_index_.swap(other.name_to_index_);
  id_to_index_.swap(other.id_to_index_);
  std::swap(has_nullables_, other.has_nullables_);
  std::swap(has_statics_, other.has_statics_);
  std::swap(table_properties_, other.table_properties_);
  std::swap(cotable_id_, other.cotable_id_);
  std::swap(pgtable_id_, other.pgtable_id_);

  // Schema cannot have both, cotable ID or pgtable ID.
  DCHECK(cotable_id_.IsNil() || pgtable_id_ == 0);
}

Status Schema::Reset(const vector<ColumnSchema>& cols,
                     const vector<ColumnId>& ids,
                     int key_columns,
                     const TableProperties& table_properties,
                     const Uuid& cotable_id,
                     const PgTableOid pgtable_id) {
  cols_ = cols;
  num_key_columns_ = key_columns;
  num_hash_key_columns_ = 0;
  table_properties_ = table_properties;
  cotable_id_ = cotable_id;
  pgtable_id_ = pgtable_id;

  // Determine whether any column is nullable or static, and count number of hash columns.
  has_nullables_ = false;
  has_statics_ = false;
  for (const ColumnSchema& col : cols_) {
    if (col.is_hash_key()) {
      num_hash_key_columns_++;
    }
    if (col.is_nullable()) {
      has_nullables_ = true;
    }
    if (col.is_static()) {
      has_statics_ = true;
    }
  }

  if (PREDICT_FALSE(key_columns > cols_.size())) {
    return STATUS(InvalidArgument,
      "Bad schema", "More key columns than columns");
  }

  if (PREDICT_FALSE(key_columns < 0)) {
    return STATUS(InvalidArgument,
      "Bad schema", "Cannot specify a negative number of key columns");
  }

  if (PREDICT_FALSE(!ids.empty() && ids.size() != cols_.size())) {
    return STATUS(InvalidArgument, "Bad schema",
      "The number of ids does not match with the number of columns");
  }

  if (PREDICT_FALSE(!cotable_id.IsNil() && pgtable_id > 0)) {
    return STATUS(InvalidArgument,
                  "Bad schema", "Cannot have both cotable ID and pgtable ID");
  }

  // Verify that the key columns are not nullable nor static
  for (int i = 0; i < key_columns; ++i) {
    if (PREDICT_FALSE(cols_[i].is_nullable())) {
      return STATUS(InvalidArgument,
        "Bad schema", strings::Substitute("Nullable key columns are not "
                                          "supported: $0", cols_[i].name()));
    }
    if (PREDICT_FALSE(cols_[i].is_static())) {
      return STATUS(InvalidArgument,
        "Bad schema", strings::Substitute("Static key columns are not "
                                          "allowed: $0", cols_[i].name()));
    }
    if (PREDICT_FALSE(cols_[i].is_counter())) {
      return STATUS(InvalidArgument,
        "Bad schema", strings::Substitute("Counter key columns are not allowed: $0",
                                          cols_[i].name()));
    }
  }

  // Calculate the offset of each column in the row format.
  col_offsets_.reserve(cols_.size() + 1);  // Include space for total byte size at the end.
  size_t off = 0;
  size_t i = 0;
  name_to_index_.clear();
  for (const ColumnSchema &col : cols_) {
    // The map uses the 'name' string from within the ColumnSchema object.
    if (!InsertIfNotPresent(&name_to_index_, col.name(), i++)) {
      return STATUS(InvalidArgument, "Duplicate column name", col.name());
    }

    col_offsets_.push_back(off);
    off += col.type_info()->size();
  }

  // Add an extra element on the end for the total
  // byte size
  col_offsets_.push_back(off);

  // Initialize IDs mapping
  col_ids_ = ids;
  id_to_index_.clear();
  max_col_id_ = 0;
  for (int i = 0; i < ids.size(); ++i) {
    if (ids[i] > max_col_id_) {
      max_col_id_ = ids[i];
    }
    id_to_index_.set(ids[i], i);
  }

  // Ensure clustering columns have a default sorting type of 'ASC' if not specified.
  for (int i = num_hash_key_columns_; i < num_key_columns(); i++) {
    ColumnSchema& col = cols_[i];
    if (col.sorting_type() == ColumnSchema::SortingType::kNotSpecified) {
      col.set_sorting_type(ColumnSchema::SortingType::kAscending);
    }
  }

  return Status::OK();
}

Status Schema::CreateProjectionByNames(const std::vector<GStringPiece>& col_names,
                                       Schema* out, size_t num_key_columns) const {
  vector<ColumnId> ids;
  vector<ColumnSchema> cols;
  for (const GStringPiece& name : col_names) {
    int idx = find_column(name);
    if (idx == -1) {
      return STATUS(NotFound, "column not found", name);
    }
    if (has_column_ids()) {
      ids.push_back(column_id(idx));
    }
    cols.push_back(column(idx));
  }
  return out->Reset(cols, ids, num_key_columns, TableProperties(), cotable_id_, pgtable_id_);
}

Status Schema::CreateProjectionByIdsIgnoreMissing(const std::vector<ColumnId>& col_ids,
                                                  Schema* out) const {
  vector<ColumnSchema> cols;
  vector<ColumnId> filtered_col_ids;
  for (ColumnId id : col_ids) {
    int idx = find_column_by_id(id);
    if (idx == -1) {
      continue;
    }
    cols.push_back(column(idx));
    filtered_col_ids.push_back(id);
  }
  return out->Reset(cols, filtered_col_ids, 0, TableProperties(), cotable_id_, pgtable_id_);
}

Schema Schema::CopyWithColumnIds() const {
  CHECK(!has_column_ids());
  vector<ColumnId> ids;
  for (int32_t i = 0; i < num_columns(); i++) {
    ids.push_back(ColumnId(kFirstColumnId + i));
  }
  return Schema(cols_, ids, num_key_columns_, table_properties_, cotable_id_, pgtable_id_);
}

Schema Schema::CopyWithoutColumnIds() const {
  CHECK(has_column_ids());
  return Schema(cols_, num_key_columns_, table_properties_, cotable_id_, pgtable_id_);
}

Status Schema::VerifyProjectionCompatibility(const Schema& projection) const {
  DCHECK(has_column_ids()) << "The server schema must have IDs";

  if (projection.has_column_ids()) {
    return STATUS(InvalidArgument, "User requests should not have Column IDs");
  }

  vector<string> missing_columns;
  for (const ColumnSchema& pcol : projection.columns()) {
    int index = find_column(pcol.name());
    if (index < 0) {
      missing_columns.push_back(pcol.name());
    } else if (!pcol.EqualsType(cols_[index])) {
      // TODO: We don't support query with type adaptors yet
      return STATUS(InvalidArgument, "The column '" + pcol.name() + "' must have type " +
                                     cols_[index].TypeToString() + " found " + pcol.TypeToString());
    }
  }

  if (!missing_columns.empty()) {
    return STATUS(InvalidArgument, "Some columns are not present in the current schema",
                                   JoinStrings(missing_columns, ", "));
  }
  return Status::OK();
}

Status Schema::GetMappedReadProjection(const Schema& projection,
                                       Schema *mapped_projection) const {
  // - The user projection may have different columns from the ones on the tablet
  // - User columns non present in the tablet are considered errors
  // - The user projection is not supposed to have the defaults or the nullable
  //   information on each field. The current tablet schema is supposed to.
  RETURN_NOT_OK(VerifyProjectionCompatibility(projection));

  // Get the Projection Mapping
  vector<ColumnSchema> mapped_cols;
  vector<ColumnId> mapped_ids;

  mapped_cols.reserve(projection.num_columns());
  mapped_ids.reserve(projection.num_columns());

  for (const ColumnSchema& col : projection.columns()) {
    int index = find_column(col.name());
    DCHECK_GE(index, 0) << col.name();
    mapped_cols.push_back(cols_[index]);
    mapped_ids.push_back(col_ids_[index]);
  }

  CHECK_OK(mapped_projection->Reset(mapped_cols, mapped_ids, projection.num_key_columns()));
  return Status::OK();
}

string Schema::ToString() const {
  vector<string> col_strs;
  if (has_column_ids()) {
    for (int i = 0; i < cols_.size(); ++i) {
      col_strs.push_back(strings::Substitute("$0:$1", col_ids_[i], cols_[i].ToString()));
    }
  } else {
    for (const ColumnSchema &col : cols_) {
      col_strs.push_back(col.ToString());
    }
  }

  TablePropertiesPB tablet_properties_pb;
  table_properties_.ToTablePropertiesPB(&tablet_properties_pb);

  return StrCat("Schema [\n\t",
                JoinStrings(col_strs, ",\n\t"),
                "\n]\nproperties: ",
                tablet_properties_pb.ShortDebugString(),
                cotable_id_.IsNil() ? "" : ("\ncotable_id: " + cotable_id_.ToString()),
                pgtable_id_ == 0 ? "" : ("\npgtable_id: " + std::to_string(pgtable_id_)));
}

Status Schema::DecodeRowKey(Slice encoded_key,
                            uint8_t* buffer,
                            Arena* arena) const {
  ContiguousRow row(this, buffer);

  for (size_t col_idx = 0; col_idx < num_key_columns(); ++col_idx) {
    const ColumnSchema& col = column(col_idx);
    const KeyEncoder<faststring>& key_encoder = GetKeyEncoder<faststring>(col.type_info());
    bool is_last = col_idx == (num_key_columns() - 1);
    RETURN_NOT_OK_PREPEND(key_encoder.Decode(&encoded_key,
                                             is_last,
                                             arena,
                                             row.mutable_cell_ptr(col_idx)),
                          strings::Substitute("Error decoding composite key component '$0'",
                                              col.name()));
  }
  return Status::OK();
}

string Schema::DebugEncodedRowKey(Slice encoded_key, StartOrEnd start_or_end) const {
  if (encoded_key.empty()) {
    switch (start_or_end) {
      case START_KEY: return "<start of table>";
      case END_KEY:   return "<end of table>";
    }
  }

  Arena arena(1024, 128 * 1024);
  uint8_t* buf = reinterpret_cast<uint8_t*>(arena.AllocateBytes(key_byte_size()));
  Status s = DecodeRowKey(encoded_key, buf, &arena);
  if (!s.ok()) {
    return "<invalid key: " + s.ToString(/* no file/line */ false) + ">";
  }
  ConstContiguousRow row(this, buf);
  return DebugRowKey(row);
}

size_t Schema::memory_footprint_excluding_this() const {
  size_t size = 0;
  for (const ColumnSchema& col : cols_) {
    size += col.memory_footprint_excluding_this();
  }

  if (cols_.capacity() > 0) {
    size += malloc_usable_size(cols_.data());
  }
  if (col_ids_.capacity() > 0) {
    size += malloc_usable_size(col_ids_.data());
  }
  if (col_offsets_.capacity() > 0) {
    size += malloc_usable_size(col_offsets_.data());
  }
  size += name_to_index_bytes_;
  size += id_to_index_.memory_footprint_excluding_this();

  return size;
}

size_t Schema::memory_footprint_including_this() const {
  return malloc_usable_size(this) + memory_footprint_excluding_this();
}

Result<int> Schema::ColumnIndexByName(GStringPiece col_name) const {
  auto index = find_column(col_name);
  if (index == kColumnNotFound) {
    return STATUS_FORMAT(Corruption, "$0 not found in schema $1", col_name, name_to_index_);
  }
  return index;
}

Result<ColumnId> Schema::ColumnIdByName(const std::string& column_name) const {
  size_t column_index = find_column(column_name);
  if (column_index == Schema::kColumnNotFound) {
    return STATUS_FORMAT(NotFound, "Couldn't find column $0 in the schema", column_name);
  }
  return ColumnId(column_id(column_index));
}

ColumnId Schema::first_column_id() {
  return kFirstColumnId;
}

// ============================================================================
//  Schema Builder
// ============================================================================
void SchemaBuilder::Reset() {
  cols_.clear();
  col_ids_.clear();
  col_names_.clear();
  num_key_columns_ = 0;
  next_id_ = kFirstColumnId;
  table_properties_.Reset();
  pgtable_id_ = 0;
  cotable_id_ = Uuid(boost::uuids::nil_uuid());
}

void SchemaBuilder::Reset(const Schema& schema) {
  cols_ = schema.cols_;
  col_ids_ = schema.col_ids_;
  num_key_columns_ = schema.num_key_columns_;
  for (const auto& column : cols_) {
    col_names_.insert(column.name());
  }

  if (col_ids_.empty()) {
    for (int32_t i = 0; i < cols_.size(); ++i) {
      col_ids_.push_back(ColumnId(kFirstColumnId + i));
    }
  }
  if (col_ids_.empty()) {
    next_id_ = kFirstColumnId;
  } else {
    next_id_ = *std::max_element(col_ids_.begin(), col_ids_.end()) + 1;
  }
  table_properties_ = schema.table_properties_;
  pgtable_id_ = schema.pgtable_id_;
  cotable_id_ = schema.cotable_id_;
}

Status SchemaBuilder::AddKeyColumn(const string& name, const shared_ptr<QLType>& type) {
  return AddColumn(ColumnSchema(name, type), /* is_nullable */ true);
}

Status SchemaBuilder::AddKeyColumn(const string& name, DataType type) {
  return AddColumn(ColumnSchema(name, QLType::Create(type)), /* is_nullable */ true);
}

Status SchemaBuilder::AddHashKeyColumn(const string& name, const shared_ptr<QLType>& type) {
  return AddColumn(ColumnSchema(name, type, false, true), true);
}

Status SchemaBuilder::AddHashKeyColumn(const string& name, DataType type) {
  return AddColumn(ColumnSchema(name, QLType::Create(type), false, true), true);
}

Status SchemaBuilder::AddColumn(const string& name,
                                const std::shared_ptr<QLType>& type,
                                bool is_nullable,
                                bool is_hash_key,
                                bool is_static,
                                bool is_counter,
                                int32_t order,
                                ColumnSchema::SortingType sorting_type) {
  return AddColumn(ColumnSchema(name, type, is_nullable, is_hash_key, is_static, is_counter,
                                order, sorting_type), false);
}

Status SchemaBuilder::RemoveColumn(const string& name) {
  unordered_set<string>::const_iterator it_names;
  if ((it_names = col_names_.find(name)) == col_names_.end()) {
    return STATUS(NotFound, "The specified column does not exist", name);
  }

  col_names_.erase(it_names);
  for (int i = 0; i < cols_.size(); ++i) {
    if (name == cols_[i].name()) {
      cols_.erase(cols_.begin() + i);
      col_ids_.erase(col_ids_.begin() + i);
      if (i < num_key_columns_) {
        num_key_columns_--;
      }
      return Status::OK();
    }
  }

  LOG(FATAL) << "Should not reach here";
  return STATUS(Corruption, "Unable to remove existing column");
}

Status SchemaBuilder::RenameColumn(const string& old_name, const string& new_name) {
  unordered_set<string>::const_iterator it_names;

  // check if 'new_name' is already in use
  if ((it_names = col_names_.find(new_name)) != col_names_.end()) {
    return STATUS(AlreadyPresent, "The column already exists", new_name);
  }

  // check if the 'old_name' column exists
  if ((it_names = col_names_.find(old_name)) == col_names_.end()) {
    return STATUS(NotFound, "The specified column does not exist", old_name);
  }

  col_names_.erase(it_names);   // TODO: Should this one stay and marked as alias?
  col_names_.insert(new_name);

  for (ColumnSchema& col_schema : cols_) {
    if (old_name == col_schema.name()) {
      col_schema.set_name(new_name);
      return Status::OK();
    }
  }

  LOG(FATAL) << "Should not reach here";
  return STATUS(IllegalState, "Unable to rename existing column");
}

Status SchemaBuilder::AddColumn(const ColumnSchema& column, bool is_key) {
  if (ContainsKey(col_names_, column.name())) {
    return STATUS(AlreadyPresent, "The column already exists", column.name());
  }

  col_names_.insert(column.name());
  if (is_key) {
    cols_.insert(cols_.begin() + num_key_columns_, column);
    col_ids_.insert(col_ids_.begin() + num_key_columns_, next_id_);
    num_key_columns_++;
  } else {
    cols_.push_back(column);
    col_ids_.push_back(next_id_);
  }

  next_id_ = ColumnId(next_id_ + 1);
  return Status::OK();
}

Status SchemaBuilder::AlterProperties(const TablePropertiesPB& pb) {
  table_properties_.AlterFromTablePropertiesPB(pb);
  return Status::OK();
}


Status DeletedColumn::FromPB(const DeletedColumnPB& col, DeletedColumn* ret) {
  ret->id = col.column_id();
  ret->ht = HybridTime(col.deleted_hybrid_time());
  return Status::OK();
}

void DeletedColumn::CopyToPB(DeletedColumnPB* pb) const {
  pb->set_column_id(id);
  pb->set_deleted_hybrid_time(ht.ToUint64());
}

} // namespace yb
