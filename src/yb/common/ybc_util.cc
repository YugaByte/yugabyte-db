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

#include "yb/common/ybc_util.h"

#include <stdarg.h>
#include <fstream>

#include "yb/common/pgsql_error.h"
#include "yb/common/pgsql_protocol.pb.h"
#include "yb/common/transaction_error.h"
#include "yb/common/ybc-internal.h"

#include "yb/util/bytes_formatter.h"
#include "yb/util/debug-util.h"
#include "yb/util/env.h"
#include "yb/util/flag_tags.h"
#include "yb/util/init.h"
#include "yb/util/logging.h"
#include "yb/util/scope_exit.h"
#include "yb/util/status.h"
#include "yb/util/version_info.h"

#include "yb/util/net/net_util.h"

#include "yb/gutil/stringprintf.h"

using std::string;
DEFINE_test_flag(string, process_info_dir, string(),
                 "Directory where all postgres process will writes their PIDs and executable name");

bool yb_debug_log_docdb_requests = false;

namespace yb {

namespace {

void ChangeWorkingDir(const char* dir) {
  int chdir_result = chdir(dir);
  if (chdir_result != 0) {
    LOG(WARNING) << "Failed to change working directory to " << dir << ", error was "
                 << errno << " " << std::strerror(errno) << "!";
  }
}

void WriteCurrentProcessInfo(const string& destination_dir) {
  string executable_path;
  if (Env::Default()->GetExecutablePath(&executable_path).ok()) {
    const auto destination_file = Format("$0/$1", destination_dir, getpid());
    std::ofstream out(destination_file, std::ios_base::out);
    out << executable_path;
    if (out) {
      LOG(INFO) << "Process info is written to " << destination_file;
      return;
    }
  }
  LOG(WARNING) << "Unable to write process info to "
               << destination_dir << " dir: error " << errno << " " << std::strerror(errno);
}

Status InitGFlags(const char* argv0) {

  const char* executable_path = argv0;
  std::string executable_path_str;
  if (executable_path == nullptr) {
    RETURN_NOT_OK(Env::Default()->GetExecutablePath(&executable_path_str));
    executable_path = executable_path_str.c_str();
  }
  DSCHECK(executable_path != nullptr, RuntimeError, "Unable to get path to executable");

  // Change current working directory from postgres data dir (as set by postmaster)
  // to the one from yb-tserver so that relative paths in gflags would be resolved in the same way.
  char pg_working_dir[PATH_MAX];
  CHECK(getcwd(pg_working_dir, sizeof(pg_working_dir)) != nullptr);
  const char* yb_working_dir = getenv("YB_WORKING_DIR");
  if (yb_working_dir) {
    ChangeWorkingDir(yb_working_dir);
  }
  auto se = ScopeExit([&pg_working_dir] {
    // Restore PG data dir as current directory.
    ChangeWorkingDir(pg_working_dir);
  });

  // Also allow overriding flags on the command line using the appropriate environment variables.
  std::vector<google::CommandLineFlagInfo> flag_infos;
  google::GetAllFlags(&flag_infos);
  for (auto& flag_info : flag_infos) {
    string env_var_name = "FLAGS_" + flag_info.name;
    const char* env_var_value = getenv(env_var_name.c_str());
    if (env_var_value) {
      google::SetCommandLineOption(flag_info.name.c_str(), env_var_value);
    }
  }

  RETURN_NOT_OK(CheckCPUFlags());
  // Use InitGoogleLoggingSafeBasic() instead of InitGoogleLoggingSafe() to avoid calling
  // google::InstallFailureSignalHandler(). This will prevent interference with PostgreSQL's
  // own signal handling.
  yb::InitGoogleLoggingSafeBasic(executable_path);

  if (VLOG_IS_ON(1)) {
    for (auto& flag_info : flag_infos) {
      string env_var_name = "FLAGS_" + flag_info.name;
      const char* env_var_value = getenv(env_var_name.c_str());
      if (env_var_value) {
        VLOG(1) << "Setting flag " << flag_info.name << " to the value of the env var "
                << env_var_name << ": " << env_var_value;
      }
    }
  }

  return Status::OK();
}

} // anonymous namespace

extern "C" {

YBCStatus YBCStatus_OK = nullptr;

// Wraps Status object created by YBCStatus.
// Uses trick with AddRef::kFalse and DetachStruct, to avoid incrementing and decrementing
// ref counter.
class StatusWrapper {
 public:
  explicit StatusWrapper(YBCStatus s) : status_(s, AddRef::kFalse) {}

  ~StatusWrapper() {
    status_.DetachStruct();
  }

  Status* operator->() {
    return &status_;
  }

  Status& operator*() {
    return status_;
  }

 private:
  Status status_;
};

bool YBCStatusIsOK(YBCStatus s) {
  return StatusWrapper(s)->IsOk();
}

bool YBCStatusIsNotFound(YBCStatus s) {
  return StatusWrapper(s)->IsNotFound();
}

bool YBCStatusIsDuplicateKey(YBCStatus s) {
  return StatusWrapper(s)->IsAlreadyPresent();
}

uint32_t YBCStatusPgsqlError(YBCStatus s) {
  StatusWrapper wrapper(s);
  const uint8_t* pg_err_ptr = wrapper->ErrorData(PgsqlErrorTag::kCategory);
  // If we have PgsqlError explicitly set, we decode it
  YBPgErrorCode result = pg_err_ptr != nullptr ? PgsqlErrorTag::Decode(pg_err_ptr)
                                               : YBPgErrorCode::YB_PG_INTERNAL_ERROR;

  // If the error is the default generic YB_PG_INTERNAL_ERROR (as we also set in AsyncRpc::Failed)
  // then we try to deduce it from a transaction error.
  if (result == YBPgErrorCode::YB_PG_INTERNAL_ERROR) {
    const uint8_t* txn_err_ptr = wrapper->ErrorData(TransactionErrorTag::kCategory);
    if (txn_err_ptr != nullptr) {
      switch (TransactionErrorTag::Decode(txn_err_ptr)) {
        case TransactionErrorCode::kAborted: FALLTHROUGH_INTENDED;
        case TransactionErrorCode::kReadRestartRequired: FALLTHROUGH_INTENDED;
        case TransactionErrorCode::kConflict:
          result = YBPgErrorCode::YB_PG_T_R_SERIALIZATION_FAILURE;
          break;
        case TransactionErrorCode::kSnapshotTooOld:
          result = YBPgErrorCode::YB_PG_SNAPSHOT_TOO_OLD;
          break;
        case TransactionErrorCode::kNone: FALLTHROUGH_INTENDED;
        default:
          result = YBPgErrorCode::YB_PG_INTERNAL_ERROR;
      }
    }
  }
  return static_cast<uint32_t>(result);
}

uint16_t YBCStatusTransactionError(YBCStatus s) {
  const TransactionError txn_err(*StatusWrapper(s));
  return static_cast<uint16_t>(txn_err.value());
}

void YBCFreeStatus(YBCStatus s) {
  FreeYBCStatus(s);
}

size_t YBCStatusMessageLen(YBCStatus s) {
  return StatusWrapper(s)->message().size();
}

const char* YBCStatusMessageBegin(YBCStatus s) {
  return StatusWrapper(s)->message().cdata();
}

const char* YBCStatusCodeAsCString(YBCStatus s) {
  return StatusWrapper(s)->CodeAsCString();
}

char* DupYBStatusMessage(YBCStatus status, bool message_only) {
  const char* const code_as_cstring = YBCStatusCodeAsCString(status);
  const size_t code_strlen = strlen(code_as_cstring);
  const size_t status_len = YBCStatusMessageLen(status);
  size_t sz = code_strlen + status_len + 3;
  if (message_only) {
    sz -= 2 + code_strlen;
  }
  char* const msg_buf = reinterpret_cast<char*>(YBCPAlloc(sz));
  char* pos = msg_buf;
  if (!message_only) {
    memcpy(msg_buf, code_as_cstring, code_strlen);
    pos += code_strlen;
    *pos++ = ':';
    *pos++ = ' ';
  }
  memcpy(pos, YBCStatusMessageBegin(status), status_len);
  pos[status_len] = 0;
  return msg_buf;
}

bool YBCIsRestartReadError(uint16_t txn_errcode) {
  return txn_errcode == static_cast<uint16_t>(TransactionErrorCode::kReadRestartRequired);
}

YBCStatus YBCInitGFlags(const char* argv0) {
  return ToYBCStatus(yb::InitGFlags(argv0));
}

YBCStatus YBCInit(const char* argv0,
                  YBCPAllocFn palloc_fn,
                  YBCCStringToTextWithLenFn cstring_to_text_with_len_fn) {
  YBCSetPAllocFn(palloc_fn);
  if (cstring_to_text_with_len_fn) {
    YBCSetCStringToTextWithLenFn(cstring_to_text_with_len_fn);
  }
  auto status = yb::InitGFlags(argv0);
  if (status.ok() && !FLAGS_TEST_process_info_dir.empty()) {
    WriteCurrentProcessInfo(FLAGS_TEST_process_info_dir);
  }
  return ToYBCStatus(status);
}

void YBCLogImpl(
    google::LogSeverity severity,
    const char* file,
    int line,
    bool with_stack_trace,
    const char* format,
    ...) {
  va_list argptr;
  va_start(argptr, format); \
  string buf;
  StringAppendV(&buf, format, argptr);
  va_end(argptr);
  google::LogMessage log_msg(file, line, severity);
  log_msg.stream() << buf;
  if (with_stack_trace) {
    log_msg.stream() << "\n" << yb::GetStackTrace();
  }
}

const char* YBCFormatBytesAsStr(const char* data, size_t size) {
  return YBCPAllocStdString(FormatBytesAsStr(data, size));
}

const char* YBCGetStackTrace() {
  return YBCPAllocStdString(yb::GetStackTrace());
}

void YBCResolveHostname() {
  string fqdn;
  auto status = GetFQDN(&fqdn);
  if (!status.ok()) {
    LOG(WARNING) << "Failed to get fully qualified domain name of the local hostname: "
                 << status;
  }
}

} // extern "C"

} // namespace yb
