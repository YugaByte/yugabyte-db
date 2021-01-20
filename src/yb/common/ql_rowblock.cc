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
//
// This file contains the classes that represent a QL row and a row block.

#include "yb/common/ql_rowblock.h"

#include "yb/util/bfql/directory.h"
#include "yb/util/bfql/bfql.h"

#include "yb/common/ql_value.h"
#include "yb/common/wire_protocol.h"

namespace yb {

using std::shared_ptr;
using std::string;

//----------------------------------------- QL row ----------------------------------------
QLRow::QLRow(const shared_ptr<const Schema>& schema)
    : schema_(schema), values_(schema->num_columns()) {
}

QLRow::QLRow(const QLRow& other) : schema_(other.schema_), values_(other.values_) {
}

QLRow::QLRow(QLRow&& other)
    : schema_(std::move(other.schema_)), values_(std::move(other.values_)) {
}

QLRow::~QLRow() {
}

void QLRow::Serialize(const QLClient client, faststring* buffer) const {
  for (size_t col_idx = 0; col_idx < schema_->num_columns(); ++col_idx) {
    values_[col_idx].Serialize(column_type(col_idx), client, buffer);
  }
}

Status QLRow::Deserialize(const QLClient client, Slice* data) {
  for (size_t col_idx = 0; col_idx < schema_->num_columns(); ++col_idx) {
    RETURN_NOT_OK(values_[col_idx].Deserialize(column_type(col_idx), client, data));
  }
  return Status::OK();
}

string QLRow::ToString() const {
  string s = "{ ";
  for (size_t col_idx = 0; col_idx < schema_->num_columns(); ++col_idx) {
    if (col_idx > 0) {
      s+= ", ";
    }
    s += values_[col_idx].ToString();
  }
  s += " }";
  return s;
}

QLRow& QLRow::operator=(const QLRow& other) {
  this->~QLRow();
  new(this) QLRow(other);
  return *this;
}

QLRow& QLRow::operator=(QLRow&& other) {
  this->~QLRow();
  new(this) QLRow(other);
  return *this;
}

const QLValue& QLRow::column(const size_t col_idx) const {
  return values_[col_idx];
}

QLValue* QLRow::mutable_column(const size_t col_idx) {
  return &values_[col_idx];
}

void QLRow::SetColumnValues(const std::vector<QLValue>& column_values) {
  values_ = column_values;
}

void QLRow::SetColumn(size_t col_idx, QLValuePB value) {
  values_[col_idx] = std::move(value);
}

//-------------------------------------- QL row block --------------------------------------
QLRowBlock::QLRowBlock(const Schema& schema, const vector<ColumnId>& column_ids)
    : schema_(new Schema()) {
  // TODO: is there a better way to report errors here?
  CHECK_OK(schema.CreateProjectionByIdsIgnoreMissing(column_ids, schema_.get()));
}

QLRowBlock::QLRowBlock(const Schema& schema) : schema_(new Schema(schema)) {
}

QLRowBlock::~QLRowBlock() {
}

QLRow& QLRowBlock::Extend() {
  rows_.emplace_back(schema_);
  return rows_.back();
}
void QLRowBlock::Reserve(size_t size) {
  rows_.reserve(size);
}

Status QLRowBlock::AddRow(const QLRow& row) {
  // TODO: check for schema compatibility between QLRow and QLRowBlock.
  rows_.push_back(row);
  return Status::OK();
}

string QLRowBlock::ToString() const {
  string s = "{ ";
  for (size_t i = 0; i < rows_.size(); i++) {
    if (i > 0) { s+= ", "; }
    s += rows_[i].ToString();
  }
  s += " }";
  return s;
}

void QLRowBlock::Serialize(const QLClient client, faststring* buffer) const {
  CHECK_EQ(client, YQL_CLIENT_CQL);
  CQLEncodeLength(rows_.size(), buffer);
  for (const auto& row : rows_) {
    row.Serialize(client, buffer);
  }
}

Status QLRowBlock::Deserialize(const QLClient client, Slice* data) {
  CHECK_EQ(client, YQL_CLIENT_CQL);
  int32_t count = 0;
  RETURN_NOT_OK(CQLDecodeNum(sizeof(count), NetworkByteOrder::Load32, data, &count));

  for (int32_t i = 0; i < count; ++i) {
    RETURN_NOT_OK(Extend().Deserialize(client, data));
  }
  if (!data->empty()) {
    return STATUS(Corruption, "Extra data at the end of row block");
  }
  return Status::OK();
}

Result<size_t> QLRowBlock::GetRowCount(const QLClient client, const string& data) {
  CHECK_EQ(client, YQL_CLIENT_CQL);
  Slice slice(data);
  return VERIFY_RESULT(CQLDecodeLength(&slice));
}

Status QLRowBlock::AppendRowsData(const QLClient client, const string& src, string* dst) {
  CHECK_EQ(client, YQL_CLIENT_CQL);
  Slice src_slice(src);
  const int32_t src_cnt = VERIFY_RESULT(CQLDecodeLength(&src_slice));
  if (src_cnt > 0) {
    Slice dst_slice(*dst);
    int32_t dst_cnt = VERIFY_RESULT(CQLDecodeLength(&dst_slice));
    if (dst_cnt == 0) {
      *dst = src;
    } else {
      dst->append(util::to_char_ptr(src_slice.data()), src_slice.size());
      dst_cnt += src_cnt;
      CQLEncodeLength(dst_cnt, &(*dst)[0]);
    }
  }
  return Status::OK();
}

string QLRowBlock::ZeroRowsData(const QLClient client) {
  CHECK_EQ(client, YQL_CLIENT_CQL);
  return string(sizeof(int32_t), 0); // Encode 32-bit 0 length.
}

} // namespace yb
