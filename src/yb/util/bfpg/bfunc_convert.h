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
//
// This module defines of C++ functions to support "cast" operators. Note that for "cast" operators,
// all arguments must of exact types. Type resolution will raise error if an argument type is
// only compatible / convertible but not equal.
//   Example: cast(int64 to string) will accept only int64 and string but not int32 and string.
//
// The conversion routines can be use for either or both of two different purposes.
// - Converting the value from one type to another.
// - Converting the value from one data representation to another.
//
// Once written, this function should not be changed to avoid compatibility issues. That is,
// server might runs one version while client use a different version of this function.
//
// See the header of file "/util/bfpg/bfpg.h" for overall info.
//--------------------------------------------------------------------------------------------------

#ifndef YB_UTIL_BFPG_BFUNC_CONVERT_H_
#define YB_UTIL_BFPG_BFUNC_CONVERT_H_

#include <iostream>
#include <string>

#include "yb/util/date_time.h"
#include "yb/util/logging.h"
#include "yb/util/net/inetaddress.h"
#include "yb/util/status.h"
#include "yb/util/stol_utils.h"

namespace yb {
namespace bfpg {

//--------------------------------------------------------------------------------------------------
// The following functions are for numeric conversions.

// Conversion for int8.
template<typename PTypePtr, typename RTypePtr>
Status ConvertI8ToI8(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_int8_value(source->int8_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI8ToI16(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_int16_value(source->int8_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI8ToI32(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_int32_value(source->int8_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI8ToI64(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_int64_value(source->int8_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI8ToFloat(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_float_value(source->int8_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI8ToDouble(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_double_value(source->int8_value());
  }
  return Status::OK();
}

// Conversion from int16 to others.
template<typename PTypePtr, typename RTypePtr>
Status ConvertI16ToI8(PTypePtr source, RTypePtr target) {
  // TODO(neil) Overflow? When we truely support expressions, these loose-ends must be fixed.
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_int8_value(source->int16_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI16ToI16(PTypePtr source, RTypePtr target) {
  // TODO(neil) Overflow? When we truely support expressions, these loose-ends must be fixed.
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_int16_value(source->int16_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI16ToI32(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_int32_value(source->int16_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI16ToI64(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_int64_value(source->int16_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI16ToFloat(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_float_value(source->int16_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI16ToDouble(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_double_value(source->int16_value());
  }
  return Status::OK();
}

// Conversion from int32 to others.
template<typename PTypePtr, typename RTypePtr>
Status ConvertI32ToI8(PTypePtr source, RTypePtr target) {
  // TODO(neil) Overflow? When we truely support expressions, these loose-ends must be fixed.
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_int8_value(source->int32_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI32ToI16(PTypePtr source, RTypePtr target) {
  // TODO(neil) Overflow? When we truely support expressions, these loose-ends must be fixed.
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_int16_value(source->int32_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI32ToI32(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_int32_value(source->int32_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI32ToI64(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_int64_value(source->int32_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI32ToFloat(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_float_value(source->int32_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI32ToDouble(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_double_value(source->int32_value());
  }
  return Status::OK();
}

// Conversion from int64 to others.
template<typename PTypePtr, typename RTypePtr>
Status ConvertI64ToI8(PTypePtr source, RTypePtr target) {
  // TODO(neil) Overflow? When we truely support expressions, these loose-ends must be fixed.
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_int8_value(source->int64_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI64ToI16(PTypePtr source, RTypePtr target) {
  // TODO(neil) Overflow? When we truely support expressions, these loose-ends must be fixed.
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_int16_value(source->int64_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI64ToI32(PTypePtr source, RTypePtr target) {
  // TODO(neil) Overflow? When we truely support expressions, these loose-ends must be fixed.
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_int32_value(source->int64_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI64ToI64(PTypePtr source, RTypePtr target) {
  // TODO(neil) Overflow? When we truely support expressions, these loose-ends must be fixed.
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_int64_value(source->int64_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI64ToFloat(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_float_value(source->int64_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI64ToDouble(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_double_value(source->int64_value());
  }
  return Status::OK();
}

// Conversion from float to others.
template<typename PTypePtr, typename RTypePtr>
Status ConvertFloatToFloat(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_float_value(source->float_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertFloatToDouble(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_double_value(source->float_value());
  }
  return Status::OK();
}

// Conversion from double to others.
template<typename PTypePtr, typename RTypePtr>
Status ConvertDoubleToFloat(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_float_value(source->double_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertDoubleToDouble(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_double_value(source->double_value());
  }
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------
// The following functions are for timestamp conversion.
template<typename PTypePtr, typename RTypePtr>
Status ConvertTimestampToI64(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_int64_value(source->timestamp_value().ToInt64());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertI64ToTimestamp(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_timestamp_value(DateTime::TimestampFromInt(source->int64_value()).ToInt64());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertTimestampToString(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_string_value(source->timestamp_value().ToString());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertStringToTimestamp(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    Timestamp ts;
    RETURN_NOT_OK(DateTime::TimestampFromString(source->string_value(), &ts));
    target->set_timestamp_value(ts.ToInt64());
  }
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------
// The following functions are for string conversion.
template<typename PTypePtr, typename RTypePtr>
Status ConvertStringToString(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_string_value(source->string_value());
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertStringToInet(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_inetaddress_value(InetAddress(
        VERIFY_RESULT(HostToAddress(source->string_value()))));
  }
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------
// The following functions are for boolean conversion.
template<typename PTypePtr, typename RTypePtr>
Status ConvertBoolToBool(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    target->set_bool_value(source->bool_value());
  }
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------
// The following functions are for conversions to blob / binary from other datatypes.

template<typename PTypePtr, typename RTypePtr>
Status ConvertStringToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBoolToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertInt8ToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertInt16ToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertInt32ToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertInt64ToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertVarintToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertFloatToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertDoubleToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertDecimalToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertDateToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertTimeToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertTimestampToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertUuidToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertTimeuuidToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertInetToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertListToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertMapToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertSetToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertTupleToBlob(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

// The following functions are for conversions from blob / binary to other datatypes.

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToString(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToBool(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToInt8(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToInt16(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToInt32(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToInt64(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToVarint(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToFloat(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToDouble(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToDecimal(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToDate(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToTime(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToTimestamp(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToUuid(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToTimeuuid(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToInet(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToList(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToMap(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToSet(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertBlobToTuple(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

//--------------------------------------------------------------------------------------------------
// The following functions are for conversions between date-time datatypes.
template<typename PTypePtr, typename RTypePtr>
Status ConvertTimeuuidToDate(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertTimestampToDate(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertTimeuuidToTime(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertTimestampToTime(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertDateToTimestamp(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertTimeuuidToTimestamp(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertDateToUnixTimestamp(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertTimestampToUnixTimestamp(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertTimeuuidToUnixTimestamp(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertToMaxTimeuuid(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertToMinTimeuuid(PTypePtr source, RTypePtr target) {
  return STATUS(RuntimeError, "Not yet implemented");
}

//--------------------------------------------------------------------------------------------------
// The following functions are for conversions from VarInt to the other numeric types.

template<typename PTypePtr, typename RTypePtr>
Status ConvertVarintToI8(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    int64_t val = VERIFY_RESULT(source->varint_value().ToInt64());
    if (val < INT8_MIN || val > INT8_MAX) {
      return STATUS(InvalidArgument, "VarInt cannot be converted to int8 due to overflow");
    }
    target->set_int8_value(val);
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertVarintToI16(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    int64_t val = VERIFY_RESULT(source->varint_value().ToInt64());
    if (val < INT16_MIN || val > INT16_MAX) {
      return STATUS(InvalidArgument, "VarInt cannot be converted to int16 due to overflow");
    }
    target->set_int16_value(val);
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertVarintToI32(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    int64_t val = VERIFY_RESULT(source->varint_value().ToInt64());
    if (val < INT32_MIN || val > INT32_MAX) {
      return STATUS(InvalidArgument, "VarInt cannot be converted to int32 due to overflow");
    }
    target->set_int32_value(val);
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertVarintToI64(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    int64_t val = VERIFY_RESULT(source->varint_value().ToInt64());
    target->set_int64_value(val);
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertVarintToFloat(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    // This may lose precision, it should return the closest float value to the input number.
    target->set_float_value(static_cast<float>(VERIFY_RESULT(CheckedStold(
        source->varint_value().ToString()))));
  }
  return Status::OK();
}

template<typename PTypePtr, typename RTypePtr>
Status ConvertVarintToDouble(PTypePtr source, RTypePtr target) {
  if (source->IsNull()) {
    target->SetNull();
  } else {
    // This may lose precision, it should return the closest double value to the input number.
    target->set_double_value(VERIFY_RESULT(CheckedStold(
        source->varint_value().ToString())));
  }
  return Status::OK();
}

} // namespace bfpg
} // namespace yb

#endif  // YB_UTIL_BFPG_BFUNC_CONVERT_H_
