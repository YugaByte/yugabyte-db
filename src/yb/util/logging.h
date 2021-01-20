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
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
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

#ifndef YB_UTIL_LOGGING_H
#define YB_UTIL_LOGGING_H

#include <fcntl.h>

#include <mutex>
#include <string>

#include <glog/logging.h>
#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/stringize.hpp>

#include "yb/gutil/atomicops.h"
#include "yb/gutil/dynamic_annotations.h"
#include "yb/gutil/walltime.h"
#include "yb/util/fault_injection.h"
#include "yb/util/logging_callback.h"
#include "yb/util/monotime.h"

////////////////////////////////////////////////////////////////////////////////
// Throttled logging support
////////////////////////////////////////////////////////////////////////////////

// Logs a message throttled to appear at most once every 'n_secs' seconds to
// the given severity.
//
// The log message may include the special token 'THROTTLE_MSG' which expands
// to either an empty string or '[suppressed <n> similar messages]'.
//
// Example usage:
//   YB_LOG_EVERY_N_SECS(WARNING, 1) << "server is low on memory" << THROTTLE_MSG;
#define YB_LOG_EVERY_N_SECS(severity, n_secs) \
  static yb::logging_internal::LogThrottler LOG_THROTTLER;  \
  int num_suppressed = 0; \
  if (LOG_THROTTLER.ShouldLog(n_secs, &num_suppressed)) \
    BOOST_PP_CAT(GOOGLE_LOG_, severity)(num_suppressed).stream()

#define YB_LOG_WITH_PREFIX_EVERY_N_SECS(severity, n_secs) \
    YB_LOG_EVERY_N_SECS(severity, n_secs) << LogPrefix()

#define YB_LOG_WITH_PREFIX_UNLOCKED_EVERY_N_SECS(severity, n_secs) \
    YB_LOG_EVERY_N_SECS(severity, n_secs) << LogPrefixUnlocked()

// Logs a messages with 2 different severities. By default used severity1, but if during
// duration there were more than count messages, then it will use severity2.
#define YB_LOG_HIGHER_SEVERITY_WHEN_TOO_MANY(severity1, severity2, duration, count) \
  static yb::logging_internal::LogRateThrottler LOG_THROTTLER(duration, count);  \
  google::LogMessage( \
    __FILE__, __LINE__, \
    LOG_THROTTLER.TooMany() ? BOOST_PP_CAT(google::GLOG_, severity2) \
                            : BOOST_PP_CAT(google::GLOG_, severity1)).stream()

namespace yb {
enum PRIVATE_ThrottleMsg {THROTTLE_MSG};
} // namespace yb

////////////////////////////////////////////////////////////////////////////////
// Versions of glog macros for "LOG_EVERY" and "LOG_FIRST" that annotate the
// benign races on their internal static variables.
////////////////////////////////////////////////////////////////////////////////

// The "base" macros.
#define YB_SOME_KIND_OF_LOG_EVERY_N(severity, n, what_to_do) \
  static int LOG_OCCURRENCES = 0, LOG_OCCURRENCES_MOD_N = 0; \
  ANNOTATE_BENIGN_RACE(&LOG_OCCURRENCES, "Logging every N is approximate"); \
  ANNOTATE_BENIGN_RACE(&LOG_OCCURRENCES_MOD_N, "Logging every N is approximate"); \
  ++LOG_OCCURRENCES; \
  if (++LOG_OCCURRENCES_MOD_N > n) LOG_OCCURRENCES_MOD_N -= n; \
  if (LOG_OCCURRENCES_MOD_N == 1) \
    google::LogMessage( \
        __FILE__, __LINE__, google::GLOG_ ## severity, LOG_OCCURRENCES, \
        &what_to_do).stream()

#define YB_SOME_KIND_OF_LOG_IF_EVERY_N(severity, condition, n, what_to_do) \
  static int LOG_OCCURRENCES = 0, LOG_OCCURRENCES_MOD_N = 0; \
  ANNOTATE_BENIGN_RACE(&LOG_OCCURRENCES, "Logging every N is approximate"); \
  ANNOTATE_BENIGN_RACE(&LOG_OCCURRENCES_MOD_N, "Logging every N is approximate"); \
  ++LOG_OCCURRENCES; \
  if (condition && \
      ((LOG_OCCURRENCES_MOD_N=(LOG_OCCURRENCES_MOD_N + 1) % n) == (1 % n))) \
    google::LogMessage( \
        __FILE__, __LINE__, google::GLOG_ ## severity, LOG_OCCURRENCES, \
                 &what_to_do).stream()

#define YB_SOME_KIND_OF_PLOG_EVERY_N(severity, n, what_to_do) \
  static int LOG_OCCURRENCES = 0, LOG_OCCURRENCES_MOD_N = 0; \
  ANNOTATE_BENIGN_RACE(&LOG_OCCURRENCES, "Logging every N is approximate"); \
  ANNOTATE_BENIGN_RACE(&LOG_OCCURRENCES_MOD_N, "Logging every N is approximate"); \
  ++LOG_OCCURRENCES; \
  if (++LOG_OCCURRENCES_MOD_N > n) LOG_OCCURRENCES_MOD_N -= n; \
  if (LOG_OCCURRENCES_MOD_N == 1) \
    google::ErrnoLogMessage( \
        __FILE__, __LINE__, google::GLOG_ ## severity, LOG_OCCURRENCES, \
        &what_to_do).stream()

#define YB_SOME_KIND_OF_LOG_FIRST_N(severity, n, what_to_do) \
  static uint64_t LOG_OCCURRENCES = 0; \
  ANNOTATE_BENIGN_RACE(&LOG_OCCURRENCES, "Logging the first N is approximate"); \
  if (LOG_OCCURRENCES++ < n) \
    google::LogMessage( \
      __FILE__, __LINE__, google::GLOG_ ## severity, LOG_OCCURRENCES, \
      &what_to_do).stream()

// The direct user-facing macros.
#define YB_LOG_EVERY_N(severity, n) \
  static_assert(google::GLOG_ ## severity < google::NUM_SEVERITIES, \
                "Invalid requested log severity"); \
  YB_SOME_KIND_OF_LOG_EVERY_N(severity, (n), google::LogMessage::SendToLog)

#define YB_LOG_WITH_PREFIX_EVERY_N(severity, n) YB_LOG_EVERY_N(severity, n) << LogPrefix()
#define YB_LOG_WITH_PREFIX_UNLOCKED_EVERY_N(severity, n) \
    YB_LOG_EVERY_N(severity, n) << LogPrefixUnlocked()

#define YB_SYSLOG_EVERY_N(severity, n) \
  YB_SOME_KIND_OF_LOG_EVERY_N(severity, (n), google::LogMessage::SendToSyslogAndLog)

#define YB_PLOG_EVERY_N(severity, n) \
  YB_SOME_KIND_OF_PLOG_EVERY_N(severity, (n), google::LogMessage::SendToLog)

#define YB_LOG_FIRST_N(severity, n) \
  YB_SOME_KIND_OF_LOG_FIRST_N(severity, (n), google::LogMessage::SendToLog)

#define YB_LOG_IF_EVERY_N(severity, condition, n) \
  YB_SOME_KIND_OF_LOG_IF_EVERY_N(severity, (condition), (n), google::LogMessage::SendToLog)

// We also disable the un-annotated glog macros for anyone who includes this header.
#undef LOG_EVERY_N
#define LOG_EVERY_N(severity, n) \
  GOOGLE_GLOG_COMPILE_ASSERT(false, "LOG_EVERY_N is deprecated. Please use YB_LOG_EVERY_N.")

#undef SYSLOG_EVERY_N
#define SYSLOG_EVERY_N(severity, n) \
  GOOGLE_GLOG_COMPILE_ASSERT(false, "SYSLOG_EVERY_N is deprecated. Please use YB_SYSLOG_EVERY_N.")

#undef PLOG_EVERY_N
#define PLOG_EVERY_N(severity, n) \
  GOOGLE_GLOG_COMPILE_ASSERT(false, "PLOG_EVERY_N is deprecated. Please use YB_PLOG_EVERY_N.")

#undef LOG_FIRST_N
#define LOG_FIRST_N(severity, n) \
  GOOGLE_GLOG_COMPILE_ASSERT(false, "LOG_FIRST_N is deprecated. Please use YB_LOG_FIRST_N.")

#undef LOG_IF_EVERY_N
#define LOG_IF_EVERY_N(severity, condition, n) \
  GOOGLE_GLOG_COMPILE_ASSERT(false, "LOG_IF_EVERY_N is deprecated. Please use YB_LOG_IF_EVERY_N.")

namespace yb {

// glog doesn't allow multiple invocations of InitGoogleLogging. This method conditionally
// calls InitGoogleLogging only if it hasn't been called before.
//
// It also takes care of installing the google failure signal handler.
void InitGoogleLoggingSafe(const char* arg);

// Like InitGoogleLoggingSafe() but stripped down: no signal handlers are
// installed, regular logging is disabled, and log events of any severity
// will be written to stderr.
//
// These properties make it attractive for us in libraries.
void InitGoogleLoggingSafeBasic(const char* arg);

// Check if Google Logging has been initialized. Can be used e.g. to determine whether to print
// something to stderr or log it. The implementation takes the logging mutex, so should not be used
// in hot codepaths.
bool IsLoggingInitialized();

// Demotes stderr logging to ERROR or higher and registers 'cb' as the
// recipient for all log events.
//
// Subsequent calls to RegisterLoggingCallback no-op (until the callback
// is unregistered with UnregisterLoggingCallback()).
void RegisterLoggingCallback(const LoggingCallback& cb);

// Unregisters a callback previously registered with
// RegisterLoggingCallback() and promotes stderr logging back to all
// severities.
//
// If no callback is registered, this is a no-op.
void UnregisterLoggingCallback();

// Returns the full pathname of the symlink to the most recent log
// file corresponding to this severity
void GetFullLogFilename(google::LogSeverity severity, std::string* filename);

// Shuts down the google logging library. Call before exit to ensure that log files are
// flushed.
void ShutdownLoggingSafe();

// Writes all command-line flags to the log at level INFO.
void LogCommandLineFlags();

// Internal function. Used by tooling for integrating with PostgreSQL C codebase.
void InitializeGoogleLogging(const char *arg);

namespace logging_internal {

// Internal implementation class used for throttling log messages.
class LogThrottler {
 public:
  LogThrottler() : num_suppressed_(0), last_ts_(0) {
    ANNOTATE_BENIGN_RACE(&last_ts_, "OK to be sloppy with log throttling");
  }

  bool ShouldLog(int n_secs, int* num_suppressed) {
    MicrosecondsInt64 ts = GetMonoTimeMicros();
    if (ts - last_ts_ < n_secs * 1e6) {
      *num_suppressed = base::subtle::NoBarrier_AtomicIncrement(&num_suppressed_, 1);
      return false;
    }
    last_ts_ = ts;
    *num_suppressed = base::subtle::NoBarrier_AtomicExchange(&num_suppressed_, 0);
    return true;
  }
 private:
  Atomic32 num_suppressed_;
  uint64_t last_ts_;
};

// Utility class that is used by YB_LOG_HIGHER_SEVERITY_WHEN_TOO_MANY macros.
class LogRateThrottler {
 public:
  LogRateThrottler(const CoarseMonoClock::duration& duration, int count)
      : duration_(duration), queue_(count) {}

  bool TooMany();
 private:
  std::mutex mutex_;
  CoarseMonoClock::duration duration_;
  // Use manual queue implementation to avoid including too heavy boost/circular_buffer
  // to this very frequently used header.
  std::vector<CoarseMonoClock::time_point> queue_;
  size_t head_ = 0;
  size_t count_ = 0;
};

} // namespace logging_internal

std::ostream& operator<<(std::ostream &os, const PRIVATE_ThrottleMsg&);

// Convenience macros to prefix log messages with some prefix, these are the unlocked
// versions and should not obtain a lock (if one is required to obtain the prefix).
// There must be a LogPrefixUnlocked()/LogPrefixLocked() method available in the current
// scope in order to use these macros.
#define LOG_WITH_PREFIX_UNLOCKED(severity) LOG(severity) << LogPrefixUnlocked()
#define VLOG_WITH_PREFIX_UNLOCKED(verboselevel) LOG_IF(INFO, VLOG_IS_ON(verboselevel)) \
  << LogPrefixUnlocked()

// Same as the above, but obtain the lock.
#define LOG_WITH_PREFIX(severity) LOG(severity) << LogPrefix()
#define VLOG_WITH_PREFIX(verboselevel) VLOG(verboselevel) << LogPrefix()
#define VLOG_WITH_FUNC(verboselevel) VLOG(verboselevel) << __func__ << ": "
#define VLOG_WITH_PREFIX_AND_FUNC(verboselevel) VLOG_WITH_PREFIX(verboselevel) << __func__ << ": "
#define DVLOG_WITH_PREFIX(verboselevel) DVLOG(verboselevel) << LogPrefix()
#define LOG_IF_WITH_PREFIX(severity, condition) LOG_IF(severity, condition) << LogPrefix()
#define VLOG_IF_WITH_PREFIX(verboselevel, condition) VLOG_IF(verboselevel, condition) << LogPrefix()

// DCHECK_ONLY_NOTNULL is like DCHECK_NOTNULL, but does not result in an unused expression in
// release mode, so it is suitable for being used as a stand-alone statement. In other words, use
// DCHECK_NOTNULL when you care about the result (e.g. when assigning it to a variable), and use
// DCHECK_ONLY_NOTNULL when you want to verify that an expression is not null in debug mode, but do
// nothing in release.
// E.g.
//   DCHECK_ONLY_NOTNULL(my_ptr)
//   SomeType* p = DCHECK_NOTNULL(SomeFunc());

#define CHECK_BETWEEN(val, lower_bound, upper_bound) \
  do { CHECK_GE(val, lower_bound); CHECK_LE(val, upper_bound); } while(false)

#ifndef NDEBUG
#define DCHECK_ONLY_NOTNULL(expr) do { DCHECK_NOTNULL(expr); } while(false)
#define DCHECK_BETWEEN(val, lower_bound, upper_bound) CHECK_BETWEEN(val, lower_bound, upper_bound)
#else
#define DCHECK_ONLY_NOTNULL(expr) do {} while(false)
#define DCHECK_BETWEEN(val, lower_bound, upper_bound) \
  GLOG_MSVC_PUSH_DISABLE_WARNING(4127) \
  while (false) \
    GLOG_MSVC_POP_WARNING() CHECK_BETWEEN(val, lower_bound, upper_bound)
#endif

// Unlike plain LOG(FATAL), here the compiler always knows we're not returning.
#define FATAL_ERROR(msg) \
  do { \
    LOG(FATAL) << (msg); \
    abort(); \
  } while (false)

void DisableCoreDumps();

// Get the path prefix for files that will contain details of a fatal crash (message and stack
// trace). This is based on the --fatal_details_path_prefix flag and the
// YB_FATAL_DETAILS_PATH_PREFIX environment variable. If neither of those are set, the result is
// based on the FATAL log path.
string GetFatalDetailsPathPrefix();

// Implements special handling for LOG(FATAL) and CHECK failures, such as disabling core dumps and
// printing the failure stack trace into a separate file.
class LogFatalHandlerSink : public google::LogSink {
 public:
  LogFatalHandlerSink();
  ~LogFatalHandlerSink();
  void send(google::LogSeverity severity, const char* /* full_filename */,
            const char* base_filename, int line, const struct tm* tm_time, const char* message,
            size_t message_len) override;
};

#define EXPR_VALUE_FOR_LOG(expr) BOOST_PP_STRINGIZE(expr) << "=" << (yb::ToString(expr))

} // namespace yb

#endif // YB_UTIL_LOGGING_H
