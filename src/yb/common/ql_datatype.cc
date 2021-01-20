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

#include <glog/logging.h>

#include "yb/common/ql_datatype.h"

namespace yb {

DataType InternalToDataType(InternalType internal_type) {
  switch (internal_type) {
    case InternalType::kInt8Value:
      return DataType::INT8;
    case InternalType::kInt16Value:
      return DataType::INT16;
    case InternalType::kInt32Value:
      return DataType::INT32;
    case InternalType::kInt64Value:
      return DataType::INT64;
    case InternalType::kUint32Value:
      return DataType::UINT32;
    case InternalType::kUint64Value:
      return DataType::UINT64;
    case InternalType::kFloatValue:
      return DataType::FLOAT;
    case InternalType::kDoubleValue:
      return DataType::DOUBLE;
    case InternalType::kDecimalValue:
      return DataType::DECIMAL;
    case InternalType::kStringValue:
      return DataType::STRING;
    case InternalType::kTimestampValue:
      return DataType::TIMESTAMP;
    case InternalType::kDateValue:
      return DataType::DATE;
    case InternalType::kTimeValue:
      return DataType::TIME;
    case InternalType::kInetaddressValue:
      return DataType::INET;
    case InternalType::kJsonbValue:
      return DataType::JSONB;
    case InternalType::kUuidValue:
      return DataType::UUID;
    case InternalType::kTimeuuidValue:
      return DataType::TIMEUUID;
    case InternalType::kBoolValue:
      return DataType::BOOL;
    case InternalType::kBinaryValue:
      return DataType::BINARY;
    case InternalType::kMapValue:
      return DataType::MAP;
    case InternalType::kSetValue:
      return DataType::SET;
    case InternalType::kListValue:
      return DataType::LIST;
    case InternalType::kVarintValue:
      return DataType::VARINT;
    case InternalType::kFrozenValue:
      return DataType::FROZEN;
    default:
      LOG(FATAL) << "Internal error: unsupported type " << internal_type;
  }
  return DataType::NULL_VALUE_TYPE;
}

std::string InternalTypeToCQLString(InternalType internal_type) {
  switch (internal_type) {
    case InternalType::VALUE_NOT_SET: return "unknown";
    case InternalType::kInt8Value: return "tinyint";
    case InternalType::kInt16Value: return "smallint";
    case InternalType::kInt32Value: return "int";
    case InternalType::kInt64Value: return "bigint";
    case InternalType::kUint32Value: return "unknown"; // No such type in YCQL.
    case InternalType::kUint64Value: return "unknown"; // No such type in YCQL.
    case InternalType::kStringValue: return "text";
    case InternalType::kBoolValue: return "boolean";
    case InternalType::kFloatValue: return "float";
    case InternalType::kDoubleValue: return "double";
    case InternalType::kBinaryValue: return "blob";
    case InternalType::kTimestampValue: return "timestamp";
    case InternalType::kDecimalValue: return "decimal";
    case InternalType::kVarintValue: return "varint";
    case InternalType::kInetaddressValue: return "inet";
    case InternalType::kJsonbValue: return "jsonb";
    case InternalType::kListValue: return "list";
    case InternalType::kMapValue: return "map";
    case InternalType::kSetValue: return "set";
    case InternalType::kUuidValue: return "uuid";
    case InternalType::kTimeuuidValue: return "timeuuid";
    case InternalType::kDateValue: return "date";
    case InternalType::kTimeValue: return "time";
    case InternalType::kFrozenValue: return "frozen";
    case InternalType::kVirtualValue: return "virtual";
  }
  LOG (FATAL) << "Invalid datatype: " << internal_type;
  return "Undefined Type";
}

} // namespace yb
