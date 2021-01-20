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

#include "yb/yql/pggate/pg_tabledesc.h"

#include "yb/client/table.h"

#include "yb/common/pg_system_attr.h"
#include "yb/common/ql_value.h"

namespace yb {
namespace pggate {

using google::protobuf::RepeatedPtrField;

PgTableDesc::PgTableDesc(std::shared_ptr<client::YBTable> pg_table)
    : table_(pg_table), table_partitions_(table_->GetVersionedPartitions()) {
  const auto& schema = pg_table->schema();
  const int num_columns = schema.num_columns();
  columns_.resize(num_columns);
  for (size_t idx = 0; idx < num_columns; idx++) {
    // Find the column descriptor.
    const auto& col = schema.Column(idx);

    // TODO(neil) Considering index columns by attr_num instead of ID.
    ColumnDesc *desc = columns_[idx].desc();
    desc->Init(idx,
               schema.ColumnId(idx),
               col.name(),
               idx < schema.num_hash_key_columns(),
               idx < schema.num_key_columns(),
               col.order() /* attr_num */,
               col.type(),
               client::YBColumnSchema::ToInternalDataType(col.type()),
               col.sorting_type());
    attr_num_map_[col.order()] = idx;
  }

  // Create virtual columns.
  column_ybctid_.Init(PgSystemAttrNum::kYBTupleId);
}

Result<PgColumn *> PgTableDesc::FindColumn(int attr_num) {
  // Find virtual columns.
  if (attr_num == static_cast<int>(PgSystemAttrNum::kYBTupleId)) {
    return &column_ybctid_;
  }

  // Find physical column.
  const auto itr = attr_num_map_.find(attr_num);
  if (itr != attr_num_map_.end()) {
    return &columns_[itr->second];
  }

  return STATUS_FORMAT(InvalidArgument, "Invalid column number $0", attr_num);
}

Status PgTableDesc::GetColumnInfo(int16_t attr_number, bool *is_primary, bool *is_hash) const {
  const auto itr = attr_num_map_.find(attr_number);
  if (itr != attr_num_map_.end()) {
    const ColumnDesc* desc = columns_[itr->second].desc();
    *is_primary = desc->is_primary();
    *is_hash = desc->is_partition();
  } else {
    *is_primary = false;
    *is_hash = false;
  }
  return Status::OK();
}

bool PgTableDesc::IsTransactional() const {
  return table_->schema().table_properties().is_transactional();
}

bool PgTableDesc::IsColocated() const {
  return table_->colocated();
}

bool PgTableDesc::IsHashPartitioned() const {
  return table_->IsHashPartitioned();
}

bool PgTableDesc::IsRangePartitioned() const {
  return table_->IsRangePartitioned();
}

const std::vector<std::string>& PgTableDesc::GetPartitions() const {
  return table_partitions_->keys;
}

int PgTableDesc::GetPartitionCount() const {
  return table_partitions_->keys.size();
}

Result<string> PgTableDesc::DecodeYbctid(const Slice& ybctid) const {
  // TODO(neil) If a partition schema can have both hash and range partitioning, this function needs
  // to be updated to return appropriate primary key.
  RSTATUS_DCHECK(!IsHashPartitioned() || !IsRangePartitioned(), InvalidArgument,
                 "Partitioning schema by both hash and range is not yet supported");

  // Use range key if there's no hash columns.
  // NOTE: Also see bug github #5832.
  if (IsRangePartitioned()) {
    // Decoding using range partitioning method.
    return ybctid.ToBuffer();
  }

  // Decoding using hash partitioning method.
  // Do not check with predicate IsHashPartitioning() for now to use existing behavior by default.
  uint16 hash_code = VERIFY_RESULT(docdb::DocKey::DecodeHash(ybctid));
  return PartitionSchema::EncodeMultiColumnHashValue(hash_code);
}

Result<int> PgTableDesc::FindPartitionIndex(const Slice& ybctid) const {
  // Find partition index based on ybctid value.
  // - Hash Partition: ybctid -> hashcode -> key -> partition index.
  // - Range Partition: ybctid == key -> partition index.
  string partition_key = VERIFY_RESULT(DecodeYbctid(ybctid));
  return client::FindPartitionStartIndex(table_partitions_->keys, partition_key);
}

Status PgTableDesc::SetScanBoundary(PgsqlReadRequestPB *req,
                                    const string& partition_lower_bound,
                                    bool lower_bound_is_inclusive,
                                    const string& partition_upper_bound,
                                    bool upper_bound_is_inclusive) {
  // Setup lower boundary.
  if (!partition_lower_bound.empty()) {
    req->mutable_lower_bound()->set_key(partition_lower_bound);
    req->mutable_lower_bound()->set_is_inclusive(lower_bound_is_inclusive);
  }

  // Setup upper boundary.
  if (!partition_upper_bound.empty()) {
    req->mutable_upper_bound()->set_key(partition_upper_bound);
    req->mutable_upper_bound()->set_is_inclusive(upper_bound_is_inclusive);
  }

  return Status::OK();
}

const client::YBTableName& PgTableDesc::table_name() const {
  return table_->name();
}

const size_t PgTableDesc::num_hash_key_columns() const {
  return table_->schema().num_hash_key_columns();
}

const size_t PgTableDesc::num_key_columns() const {
  return table_->schema().num_key_columns();
}

const size_t PgTableDesc::num_columns() const {
  return table_->schema().num_columns();
}

std::unique_ptr<client::YBPgsqlReadOp> PgTableDesc::NewPgsqlSelect() {
  return table_->NewPgsqlSelect();
}

std::unique_ptr<client::YBPgsqlWriteOp> PgTableDesc::NewPgsqlInsert() {
  return table_->NewPgsqlInsert();
}

std::unique_ptr<client::YBPgsqlWriteOp> PgTableDesc::NewPgsqlUpdate() {
  return table_->NewPgsqlUpdate();
}

std::unique_ptr<client::YBPgsqlWriteOp> PgTableDesc::NewPgsqlDelete() {
  return table_->NewPgsqlDelete();
}

std::unique_ptr<client::YBPgsqlWriteOp> PgTableDesc::NewPgsqlTruncateColocated() {
  return table_->NewPgsqlTruncateColocated();
}

}  // namespace pggate
}  // namespace yb
