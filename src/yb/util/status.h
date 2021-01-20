// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
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
// A Status encapsulates the result of an operation.  It may indicate success,
// or it may indicate an error with an associated error message.
//
// Multiple threads can invoke const methods on a Status without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same Status must use
// external synchronization.

#ifndef YB_UTIL_STATUS_H_
#define YB_UTIL_STATUS_H_

#include <atomic>
#include <memory>
#include <string>

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>

#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/tuple/elem.hpp>

#ifdef YB_HEADERS_NO_STUBS
#include "yb/gutil/macros.h"
#include "yb/gutil/port.h"
#else
#include "yb/client/stubs.h"
#endif

#include "yb/util/slice.h"
#include "yb/util/format.h"
#include "yb/util/strongly_typed_bool.h"
#include "yb/gutil/endian.h"
#include "yb/gutil/strings/substitute.h"

// Return the given status if it is not OK.
#define YB_RETURN_NOT_OK(s) do { \
    auto&& _s = (s); \
    if (PREDICT_FALSE(!_s.ok())) return MoveStatus(std::move(_s)); \
  } while (false)

// Return the given status if it is not OK, but first clone it and prepend the given message.
#define YB_RETURN_NOT_OK_PREPEND(s, msg) do { \
    auto&& _s = (s); \
    if (PREDICT_FALSE(!_s.ok())) return MoveStatus(_s).CloneAndPrepend(msg); \
  } while (0);

// Return 'to_return' if 'to_call' returns a bad status.  The substitution for 'to_return' may
// reference the variable 's' for the bad status.
#define YB_RETURN_NOT_OK_RET(to_call, to_return) do { \
    ::yb::Status s = (to_call); \
    if (PREDICT_FALSE(!s.ok())) return (to_return);  \
  } while (0);

#define YB_DFATAL_OR_RETURN_NOT_OK(s) do { \
    LOG_IF(DFATAL, !s.ok()) << s; \
    YB_RETURN_NOT_OK(s); \
  } while (0);

#define YB_DFATAL_OR_RETURN_ERROR_IF(condition, s) do { \
    if (PREDICT_FALSE(condition)) { \
      DCHECK(!s.ok()) << "Invalid OK status"; \
      LOG(DFATAL) << s; \
      return s; \
    } \
  } while (0);

// Emit a warning if 'to_call' returns a bad status.
#define YB_WARN_NOT_OK(to_call, warning_prefix) do { \
    ::yb::Status _s = (to_call); \
    if (PREDICT_FALSE(!_s.ok())) { \
      YB_LOG(WARNING) << (warning_prefix) << ": " << _s.ToString();  \
    } \
  } while (0);

#define WARN_WITH_PREFIX_NOT_OK(to_call, warning_prefix) do { \
    ::yb::Status _s = (to_call); \
    if (PREDICT_FALSE(!_s.ok())) { \
      YB_LOG(WARNING) << LogPrefix() << (warning_prefix) << ": " << _s; \
    } \
  } while (0);

// Emit a error if 'to_call' returns a bad status.
#define ERROR_NOT_OK(to_call, error_prefix) do { \
    ::yb::Status _s = (to_call); \
    if (PREDICT_FALSE(!_s.ok())) { \
      YB_LOG(ERROR) << (error_prefix) << ": " << _s.ToString();  \
    } \
  } while (0);

// Log the given status and return immediately.
#define YB_LOG_AND_RETURN(level, status) do { \
    ::yb::Status _s = (status); \
    YB_LOG(level) << _s.ToString(); \
    return _s; \
  } while (0);

// If 'to_call' returns a bad status, CHECK immediately with a logged message of 'msg' followed by
// the status.
#define YB_CHECK_OK_PREPEND(to_call, msg) do { \
  auto&& _s = (to_call); \
  YB_CHECK(_s.ok()) << (msg) << ": " << StatusToString(_s); \
  } while (0);

// If the status is bad, CHECK immediately, appending the status to the logged message.
#define YB_CHECK_OK(s) YB_CHECK_OK_PREPEND(s, "Bad status")

#define RETURN_NOT_OK           YB_RETURN_NOT_OK
#define RETURN_NOT_OK_PREPEND   YB_RETURN_NOT_OK_PREPEND
#define RETURN_NOT_OK_RET       YB_RETURN_NOT_OK_RET
// If status is not OK, this will FATAL in debug mode, or return the error otherwise.
#define DFATAL_OR_RETURN_NOT_OK YB_DFATAL_OR_RETURN_NOT_OK
#define DFATAL_OR_RETURN_ERROR_IF  YB_DFATAL_OR_RETURN_ERROR_IF
#define WARN_NOT_OK             YB_WARN_NOT_OK
#define LOG_AND_RETURN          YB_LOG_AND_RETURN
#define CHECK_OK_PREPEND        YB_CHECK_OK_PREPEND
#define CHECK_OK                YB_CHECK_OK

// These are standard glog macros.
#define YB_LOG              LOG
#define YB_CHECK            CHECK

extern "C" {

struct YBCStatusStruct;

}

namespace yb {

#define YB_STATUS_CODES \
    ((Ok, OK, 0, "OK")) \
    ((NotFound, NOT_FOUND, 1, "Not found")) \
    ((Corruption, CORRUPTION, 2, "Corruption")) \
    ((NotSupported, NOT_SUPPORTED, 3, "Not implemented")) \
    ((InvalidArgument, INVALID_ARGUMENT, 4, "Invalid argument")) \
    ((IOError, IO_ERROR, 5, "IO error")) \
    ((AlreadyPresent, ALREADY_PRESENT, 6, "Already present")) \
    ((RuntimeError, RUNTIME_ERROR, 7, "Runtime error")) \
    ((NetworkError, NETWORK_ERROR, 8, "Network error")) \
    ((IllegalState, ILLEGAL_STATE, 9, "Illegal state")) \
    ((NotAuthorized, NOT_AUTHORIZED, 10, "Not authorized")) \
    ((Aborted, ABORTED, 11, "Aborted")) \
    ((RemoteError, REMOTE_ERROR, 12, "Remote error")) \
    ((ServiceUnavailable, SERVICE_UNAVAILABLE, 13, "Service unavailable")) \
    ((TimedOut, TIMED_OUT, 14, "Timed out")) \
    ((Uninitialized, UNINITIALIZED, 15, "Uninitialized")) \
    ((ConfigurationError, CONFIGURATION_ERROR, 16, "Configuration error")) \
    ((Incomplete, INCOMPLETE, 17, "Incomplete")) \
    ((EndOfFile, END_OF_FILE, 18, "End of file")) \
    ((InvalidCommand, INVALID_COMMAND, 19, "Invalid command")) \
    ((QLError, QL_ERROR, 20, "Query error")) \
    ((InternalError, INTERNAL_ERROR, 21, "Internal error")) \
    ((Expired, EXPIRED, 22, "Operation expired")) \
    ((LeaderNotReadyToServe, LEADER_NOT_READY_TO_SERVE, 23, \
        "Leader not ready to serve requests.")) \
    ((LeaderHasNoLease, LEADER_HAS_NO_LEASE, 24, "Leader does not have a valid lease.")) \
    ((TryAgain, TRY_AGAIN_CODE, 25, "Operation failed. Try again.")) \
    ((Busy, BUSY, 26, "Resource busy")) \
    ((ShutdownInProgress, SHUTDOWN_IN_PROGRESS, 27, "Shutdown in progress")) \
    ((MergeInProgress, MERGE_IN_PROGRESS, 28, "Merge in progress")) \
    ((Combined, COMBINED_ERROR, 29, "Combined status representing multiple status failures.")) \
    ((SnapshotTooOld, SNAPSHOT_TOO_OLD, 30, "Snapshot too old")) \
    /**/

#define YB_STATUS_CODE_DECLARE(name, pb_name, value, message) \
    BOOST_PP_CAT(k, name) = value,

#define YB_STATUS_CODE_IS_FUNC(name, pb_name, value, message) \
    bool BOOST_PP_CAT(Is, name)() const { \
      return code() == BOOST_PP_CAT(k, name); \
    } \
    /**/

#define YB_STATUS_FORWARD_MACRO(r, data, tuple) data tuple

YB_STRONGLY_TYPED_BOOL(DupFileName);
YB_STRONGLY_TYPED_BOOL(AddRef);

// Extra error code assigned to status.
class StatusErrorCode {
 public:
  virtual uint8_t Category() const = 0;
  virtual size_t EncodedSize() const = 0;
  // Serialization should not be changed after error code is released, since it is
  // transferred over the wire.
  virtual uint8_t* Encode(uint8_t* out) const = 0;
  virtual std::string Message() const = 0;

  virtual ~StatusErrorCode() = default;
};

template <class Tag>
class StatusErrorCodeImpl : public StatusErrorCode {
 public:
  typedef typename Tag::Value Value;
  // Category is a part of the wire protocol.
  // So it should not be changed after first release containing this category.
  // All used categories could be listed with the following command:
  // git grep -h -F "uint8_t kCategory" | awk '{ print $6 }' | sort -n
  static constexpr uint8_t kCategory = Tag::kCategory;

  explicit StatusErrorCodeImpl(const Value& value) : value_(value) {}
  explicit StatusErrorCodeImpl(Value&& value) : value_(std::move(value)) {}

  explicit StatusErrorCodeImpl(const Status& status);

  static boost::optional<StatusErrorCodeImpl> FromStatus(const Status& status);

  uint8_t Category() const override {
    return kCategory;
  }

  size_t EncodedSize() const override {
    return Tag::EncodedSize(value_);
  }

  uint8_t* Encode(uint8_t* out) const override {
    return Tag::Encode(value_, out);
  }

  const Value& value() const {
    return value_;
  }

  std::string Message() const override {
    return Tag::ToMessage(value_);
  }

 private:
  Value value_;
};

template <class Tag>
bool operator==(const StatusErrorCodeImpl<Tag>& lhs, const StatusErrorCodeImpl<Tag>& rhs) {
  return lhs.value() == rhs.value();
}

template <class Tag>
bool operator==(const StatusErrorCodeImpl<Tag>& lhs, const typename Tag::Value& rhs) {
  return lhs.value() == rhs;
}

template <class Tag>
bool operator==(const typename Tag::Value& lhs, const StatusErrorCodeImpl<Tag>& rhs) {
  return lhs == rhs.value();
}

template <class Tag>
bool operator!=(const StatusErrorCodeImpl<Tag>& lhs, const StatusErrorCodeImpl<Tag>& rhs) {
  return lhs.value() != rhs.value();
}

template <class Tag>
bool operator!=(const StatusErrorCodeImpl<Tag>& lhs, const typename Tag::Value& rhs) {
  return lhs.value() != rhs;
}

template <class Tag>
bool operator!=(const typename Tag::Value& lhs, const StatusErrorCodeImpl<Tag>& rhs) {
  return lhs != rhs.value();
}

// Base class for all error tags that use integral representation.
// For instance time duration.
template <class Traits>
class IntegralBackedErrorTag {
 public:
  typedef typename Traits::ValueType Value;

  static Value Decode(const uint8_t* source) {
    if (!source) {
      return Value();
    }
    return Traits::FromRepresentation(
        Load<typename Traits::RepresentationType, LittleEndian>(source));
  }

  static size_t DecodeSize(const uint8_t* source) {
    return sizeof(typename Traits::RepresentationType);
  }

  static size_t EncodedSize(Value value) {
    return sizeof(typename Traits::RepresentationType);
  }

  static uint8_t* Encode(Value value, uint8_t* out) {
    Store<typename Traits::RepresentationType, LittleEndian>(out, Traits::ToRepresentation(value));
    return out + sizeof(typename Traits::RepresentationType);
  }

  static std::string DecodeToString(const uint8_t* source) {
    return Traits::ToString(Decode(source));
  }
};

class StringVectorBackedErrorTag {
 public:
  typedef typename std::vector<std::string> Value;
  typedef uint64_t SizeType;

  static Value Decode(const uint8_t* source);

  static size_t DecodeSize(const uint8_t* source) {
    return Load<SizeType, LittleEndian>(source);
  }

  static size_t EncodedSize(const Value& value);

  static uint8_t* Encode(const Value& value, uint8_t* out);

  static std::string DecodeToString(const uint8_t* source) {
    return AsString(Decode(source));
  }
};

template <class Enum>
typename std::enable_if<std::is_enum<Enum>::value, std::string>::type
IntegralToString(Enum e) {
  return std::to_string(static_cast<typename std::underlying_type<Enum>::type>(e));
}

template <class Value>
typename std::enable_if<!std::is_enum<Value>::value, std::string>::type
IntegralToString(Value value) {
  return std::to_string(value);
}

// Base class for error tags that have integral value type.
template <class Value>
class PlainIntegralTraits {
 public:
  typedef Value ValueType;
  typedef ValueType RepresentationType;

  static ValueType FromRepresentation(RepresentationType source) {
    return source;
  }

  static RepresentationType ToRepresentation(ValueType value) {
    return value;
  }

  static std::string ToString(ValueType value) {
    return IntegralToString(value);
  }
};

template <class ValueType>
struct IntegralErrorTag : public IntegralBackedErrorTag<PlainIntegralTraits<ValueType>> {
};

struct StatusCategoryDescription {
  uint8_t id = 0;
  const std::string* name = nullptr;
  std::function<size_t(const uint8_t*)> decode_size;
  std::function<std::string(const uint8_t*)> to_string;

  StatusCategoryDescription() = default;

  template <class Tag>
  static StatusCategoryDescription Make(const std::string* name_) {
    return StatusCategoryDescription{Tag::kCategory, name_, &Tag::DecodeSize, &Tag::DecodeToString};
  }
};

#ifdef __clang__
#define NODISCARD_CLASS [[nodiscard]] // NOLINT
#else
#define NODISCARD_CLASS // NOLINT
#endif

#ifndef DISABLE_STATUS_NODISCARD
#define STATUS_NODISCARD_CLASS NODISCARD_CLASS
#else
#define STATUS_NODISCARD_CLASS
#endif

class STATUS_NODISCARD_CLASS Status {
 public:
  // Wrapper class for OK status to forbid creation of Result from Status::OK in compile time
  class OK {
   public:
    operator Status() const {
      return Status();
    }
  };
  // Create a success status.
  Status() {}

  // Returns true if the status indicates success.
  bool ok() const { return state_ == nullptr; }

  // Declares set of Is* functions
  BOOST_PP_SEQ_FOR_EACH(YB_STATUS_FORWARD_MACRO, YB_STATUS_CODE_IS_FUNC, YB_STATUS_CODES)

  // Returns a text message of this status to be reported to users.
  // Returns empty string for success.
  std::string ToUserMessage(bool include_code = false) const {
    return ToString(false /* include_file_and_line */, include_code);
  }

  // Return a string representation of this status suitable for printing.
  // Returns the string "OK" for success.
  std::string ToString(bool include_file_and_line = true, bool include_code = true) const;

  // Return a string representation of the status code, without the message
  // text or posix code information.
  std::string CodeAsString() const;

  // Returned string has unlimited lifetime, and should NOT be released by the caller.
  const char* CodeAsCString() const;

  // Return the message portion of the Status. This is similar to ToString,
  // except that it does not include the stringified error code or posix code.
  //
  // For OK statuses, this returns an empty string.
  //
  // The returned Slice is only valid as long as this Status object remains
  // live and unchanged.
  Slice message() const;

  const uint8_t* ErrorData(uint8_t category) const;
  Slice ErrorCodesSlice() const;

  const char* file_name() const;
  int line_number() const;

  // Return a new Status object with the same state plus an additional leading message.
  Status CloneAndPrepend(const Slice& msg) const;

  // Same as CloneAndPrepend, but appends to the message instead.
  Status CloneAndAppend(const Slice& msg) const;

  // Same as CloneAndPrepend, but adds new error code to status.
  // If error code of the same category already present, it will be replaced with new one.
  Status CloneAndAddErrorCode(const StatusErrorCode& error_code) const;

  // Returns the memory usage of this object without the object itself. Should
  // be used when embedded inside another object.
  size_t memory_footprint_excluding_this() const;

  // Returns the memory usage of this object including the object itself.
  // Should be used when allocated on the heap.
  size_t memory_footprint_including_this() const;

  enum Code : int32_t {
    BOOST_PP_SEQ_FOR_EACH(YB_STATUS_FORWARD_MACRO, YB_STATUS_CODE_DECLARE, YB_STATUS_CODES)

    // NOTE: Remember to duplicate these constants into wire_protocol.proto and
    // and to add StatusTo/FromPB ser/deser cases in wire_protocol.cc !
    //
    // TODO: Move error codes into an error_code.proto or something similar.
  };

  // Return a new Status object with the same state except status code.
  Status CloneAndReplaceCode(Code code) const;

  Status(Code code,
         const char* file_name,
         int line_number,
         const Slice& msg,
         // Error message details. If present - would be combined as "msg: msg2".
         const Slice& msg2 = Slice(),
         const StatusErrorCode* error = nullptr,
         DupFileName dup_file_name = DupFileName::kFalse);

  Status(Code code,
         const char* file_name,
         int line_number,
         const Slice& msg,
         // Error message details. If present - would be combined as "msg: msg2".
         const Slice& msg2,
         const StatusErrorCode& error,
         DupFileName dup_file_name = DupFileName::kFalse)
      : Status(code, file_name, line_number, msg, msg2, &error, dup_file_name) {
  }

  Status(Code code,
         const char* file_name,
         int line_number,
         const StatusErrorCode& error,
         DupFileName dup_file_name = DupFileName::kFalse)
      : Status(code, file_name, line_number, error.Message(), Slice(), error, dup_file_name) {
  }

  Status(Code code,
         const char* file_name,
         int line_number,
         const Slice& msg,
         const StatusErrorCode& error,
         DupFileName dup_file_name = DupFileName::kFalse)
      : Status(code, file_name, line_number, msg, error.Message(), error, dup_file_name) {
  }


  Status(Code code,
         const char* file_name,
         int line_number,
         const Slice& msg,
         const Slice& errors,
         DupFileName dup_file_name);

  Code code() const;

  static void RegisterCategory(const StatusCategoryDescription& description);

  static const std::string& CategoryName(uint8_t category);

  // Adopt status that was previously exported to C interface.
  explicit Status(YBCStatusStruct* state, AddRef add_ref);

  // Increments state ref count and returns pointer that could be used in C interface.
  YBCStatusStruct* RetainStruct() const;

  // Reset state w/o touching ref count. Return detached pointer that could be used in C interface.
  YBCStatusStruct* DetachStruct();

 private:
  struct State;

  bool file_name_duplicated() const;

  typedef boost::intrusive_ptr<State> StatePtr;

  explicit Status(StatePtr state);

  friend void intrusive_ptr_release(State* state);
  friend void intrusive_ptr_add_ref(State* state);

  StatePtr state_;

  static_assert(sizeof(Code) == 4, "Code enum size is part of ABI");
};

class StatusCategoryRegisterer {
 public:
  explicit StatusCategoryRegisterer(const StatusCategoryDescription& description);
};

template <class Tag>
StatusErrorCodeImpl<Tag>::StatusErrorCodeImpl(const Status& status)
    : value_(Tag::Decode(status.ErrorData(Tag::kCategory))) {}

template <class Tag>
boost::optional<StatusErrorCodeImpl<Tag>> StatusErrorCodeImpl<Tag>::FromStatus(
    const Status& status) {
  const auto* error_data = status.ErrorData(Tag::kCategory);
  if (!error_data) {
    return boost::none;
  }
  return StatusErrorCodeImpl<Tag>(Tag::Decode(error_data));
}

inline Status&& MoveStatus(Status&& status) {
  return std::move(status);
}

inline const Status& MoveStatus(const Status& status) {
  return status;
}

inline std::string StatusToString(const Status& status) {
  return status.ToString();
}

inline std::ostream& operator<<(std::ostream& out, const Status& status) {
  return out << status.ToString();
}

}  // namespace yb

#define STATUS(status_type, ...) \
    (Status(Status::BOOST_PP_CAT(k, status_type), __FILE__, __LINE__, __VA_ARGS__))
#define STATUS_SUBSTITUTE(status_type, ...) \
    (Status(Status::BOOST_PP_CAT(k, status_type), \
            __FILE__, \
            __LINE__, \
            strings::Substitute(__VA_ARGS__)))

#define STATUS_FORMAT(status_type, ...) \
    (::yb::Status(::yb::Status::BOOST_PP_CAT(k, status_type), \
            __FILE__, \
            __LINE__, \
            ::yb::Format(__VA_ARGS__)))

#define STATUS_EC_FORMAT(status_type, error_code, ...) \
    (::yb::Status(::yb::Status::BOOST_PP_CAT(k, status_type), \
            __FILE__, \
            __LINE__, \
            ::yb::Format(__VA_ARGS__), error_code))

// Utility macros to perform the appropriate check. If the check fails, returns the specified
// (error) Status, with the given message.
#define SCHECK(expr, status_type, msg) do { \
    if (PREDICT_FALSE(!(expr))) return STATUS(status_type, (msg)); \
  } while (0)

#define SCHECK_FORMAT(expr, status_type, msg, ...) do { \
    if (PREDICT_FALSE(!(expr))) return STATUS_FORMAT(status_type, (msg), __VA_ARGS__); \
  } while (0)

#define SCHECK_OP(var1, op, var2, status_type, msg) \
  do { \
    auto v1_tmp = (var1); \
    auto v2_tmp = (var2); \
    if (PREDICT_FALSE(!((v1_tmp) op (v2_tmp)))) return STATUS(status_type, \
      yb::Format("$0: $1 vs. $2", (msg), v1_tmp, v2_tmp)); \
  } while (0)

#define SCHECK_EQ(var1, var2, status_type, msg) SCHECK_OP(var1, ==, var2, status_type, msg)
#define SCHECK_NE(var1, var2, status_type, msg) SCHECK_OP(var1, !=, var2, status_type, msg)
#define SCHECK_GT(var1, var2, status_type, msg) SCHECK_OP(var1, >, var2, status_type, msg)
#define SCHECK_GE(var1, var2, status_type, msg) SCHECK_OP(var1, >=, var2, status_type, msg)
#define SCHECK_LT(var1, var2, status_type, msg) SCHECK_OP(var1, <, var2, status_type, msg)
#define SCHECK_LE(var1, var2, status_type, msg) SCHECK_OP(var1, <=, var2, status_type, msg)
#define SCHECK_BOUNDS(var1, lbound, rbound, status_type, msg) \
    do { \
      SCHECK_GE(var1, lbound, status_type, msg); \
      SCHECK_LE(var1, rbound, status_type, msg); \
    } while(false)

#ifndef NDEBUG

// Debug mode ("not defined NDEBUG (non-debug-mode)" means "debug mode").
// In case the check condition is false, we will crash with a CHECK failure.

#define RSTATUS_DCHECK(expr, type, msg) DCHECK(expr) << msg
#define RSTATUS_DCHECK_EQ(var1, var2, type, msg) DCHECK_EQ(var1, var2) << msg
#define RSTATUS_DCHECK_NE(var1, var2, type, msg) DCHECK_NE(var1, var2) << msg
#define RSTATUS_DCHECK_GT(var1, var2, type, msg) DCHECK_GT(var1, var2) << msg
#define RSTATUS_DCHECK_GE(var1, var2, type, msg) DCHECK_GE(var1, var2) << msg
#define RSTATUS_DCHECK_LT(var1, var2, type, msg) DCHECK_LT(var1, var2) << msg
#define RSTATUS_DCHECK_LE(var1, var2, type, msg) DCHECK_LE(var1, var2) << msg

#else

// Release mode.
// In case the check condition is false, we will return an error status.

#define RSTATUS_DCHECK(expr, type, msg) SCHECK(expr, type, msg)
#define RSTATUS_DCHECK_EQ(var1, var2, type, msg) SCHECK_EQ(var1, var2, type, msg)
#define RSTATUS_DCHECK_NE(var1, var2, type, msg) SCHECK_NE(var1, var2, type, msg)
#define RSTATUS_DCHECK_GT(var1, var2, type, msg) SCHECK_GT(var1, var2, type, msg)
#define RSTATUS_DCHECK_GE(var1, var2, type, msg) SCHECK_GE(var1, var2, type, msg)
#define RSTATUS_DCHECK_LT(var1, var2, type, msg) SCHECK_LT(var1, var2, type, msg)
#define RSTATUS_DCHECK_LE(var1, var2, type, msg) SCHECK_LE(var1, var2, type, msg)

#endif

#ifdef YB_HEADERS_NO_STUBS
#define CHECKED_STATUS MUST_USE_RESULT ::yb::Status
#else
// Only for the build using client headers. MUST_USE_RESULT is undefined in that case.
#define CHECKED_STATUS ::yb::Status
#endif

#endif  // YB_UTIL_STATUS_H_
