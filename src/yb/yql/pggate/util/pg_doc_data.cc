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

#include "yb/yql/pggate/util/pg_doc_data.h"

#include "yb/client/client.h"

#include "yb/common/ql_value.h"

#include "yb/util/decimal.h"

namespace yb {
namespace pggate {

Status WriteColumn(const QLValuePB& col_value, faststring *buffer) {
  // Write data header.
  bool has_data = true;
  PgWireDataHeader col_header;
  if (QLValue::IsNull(col_value)) {
    col_header.set_null();
    has_data = false;
  }
  PgWire::WriteUint8(col_header.ToUint8(), buffer);

  if (!has_data) {
    return Status::OK();
  }

  switch (col_value.value_case()) {
    case InternalType::VALUE_NOT_SET:
      break;
    case InternalType::kBoolValue:
      PgWire::WriteBool(col_value.bool_value(), buffer);
      break;
    case InternalType::kInt8Value:
      PgWire::WriteInt8(col_value.int8_value(), buffer);
      break;
    case InternalType::kInt16Value:
      PgWire::WriteInt16(col_value.int16_value(), buffer);
      break;
    case InternalType::kInt32Value:
      PgWire::WriteInt32(col_value.int32_value(), buffer);
      break;
    case InternalType::kInt64Value:
      PgWire::WriteInt64(col_value.int64_value(), buffer);
      break;
    case InternalType::kUint32Value:
      PgWire::WriteUint32(col_value.uint32_value(), buffer);
      break;
    case InternalType::kUint64Value:
      PgWire::WriteUint64(col_value.uint64_value(), buffer);
      break;
    case InternalType::kFloatValue:
      PgWire::WriteFloat(col_value.float_value(), buffer);
      break;
    case InternalType::kDoubleValue:
      PgWire::WriteDouble(col_value.double_value(), buffer);
      break;
    case InternalType::kStringValue:
      PgWire::WriteText(col_value.string_value(), buffer);
      break;
    case InternalType::kBinaryValue:
      PgWire::WriteBinary(col_value.binary_value(), buffer);
      break;
    case InternalType::kDecimalValue:
      // Passing a serialized form of YB Decimal, decoding will be done in pg_expr.cc
      PgWire::WriteText(col_value.decimal_value(), buffer);
      break;
    case InternalType::kVirtualValue:
      // Expecting database to return an actual value and not a virtual one.
    case InternalType::kTimestampValue:
    case InternalType::kDateValue: // Not used for PG storage
    case InternalType::kTimeValue: // Not used for PG storage
    case InternalType::kVarintValue:
    case InternalType::kInetaddressValue:
    case InternalType::kJsonbValue:
    case InternalType::kUuidValue:
    case InternalType::kTimeuuidValue:
      // PgGate has not supported these datatypes yet.
      return STATUS_FORMAT(NotSupported,
          "Unexpected data was read from database: col_value.type()=$0", col_value.value_case());

    case InternalType::kListValue:
    case InternalType::kMapValue:
    case InternalType::kSetValue:
    case InternalType::kFrozenValue:
      // Postgres does not have these datatypes.
      return STATUS_FORMAT(Corruption,
          "Unexpected data was read from database: col_value.type()=$0", col_value.value_case());
  }

  return Status::OK();
}

//--------------------------------------------------------------------------------------------------
// Read Tuple Routine in DocDB Format (wire_protocol).
//--------------------------------------------------------------------------------------------------

void PgDocData::LoadCache(const string& cache, int64_t *total_row_count, Slice *cursor) {
  // Setup the buffer to read the next set of tuples.
  CHECK(cursor->empty()) << "Existing cache is not yet fully read";
  *cursor = cache;

  // Read the number row_count in this set.
  int64_t this_count;
  size_t read_size = ReadNumber(cursor, &this_count);
  *total_row_count = this_count;
  cursor->remove_prefix(read_size);
}

PgWireDataHeader PgDocData::ReadDataHeader(Slice *cursor) {
  // Read for NULL value.
  uint8_t header_data;
  size_t read_size = ReadNumber(cursor, &header_data);
  cursor->remove_prefix(read_size);

  return PgWireDataHeader(header_data);
}

}  // namespace pggate
}  // namespace yb
