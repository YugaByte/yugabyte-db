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
#ifndef YB_UTIL_DEBUG_UTIL_H
#define YB_UTIL_DEBUG_UTIL_H

#include <sys/types.h>

#include <string>
#include <vector>

#include "yb/gutil/strings/fastmem.h"
#include "yb/util/enums.h"
#include "yb/util/result.h"
#include "yb/util/thread.h"

namespace yb {

enum class StackTraceLineFormat {
  SHORT,
  CLION_CLICKABLE,
  SYMBOL_ONLY,
  DEFAULT = SHORT
};

// Return a list of all of the thread IDs currently running in this process.
// Not async-safe.
Status ListThreads(std::vector<pid_t>* tids);

// Set which POSIX signal number should be used internally for triggering
// stack traces. If the specified signal handler is already in use, this
// returns an error, and stack traces will be disabled.
Status SetStackTraceSignal(int signum);

// Return the stack trace of the given thread, stringified and symbolized.
//
// Note that the symbolization happens on the calling thread, not the target
// thread, so this is relatively low-impact on the target.
//
// This is safe to use against the current thread, the main thread, or any other
// thread. It requires that the target thread has not blocked POSIX signals. If
// it has, an error message will be returned.
//
// This function is thread-safe but coarsely synchronized: only one "dumper" thread
// may be active at a time.
std::string DumpThreadStack(ThreadIdForStack tid);

// Return the current stack trace, stringified.
std::string GetStackTrace(
    StackTraceLineFormat source_file_path_format = StackTraceLineFormat::DEFAULT,
    int num_top_frames_to_skip = 0);

// Return the current stack trace without the top frame. Useful for implementing functions that
// themselves have a "get/print stack trace at the call site" semantics. This is equivalent to
// calling GetStackTrace(StackTraceLineFormat::DEFAULT, 1).
inline std::string GetStackTraceWithoutTopFrame() {
  // Note that here we specify num_top_frames_to_skip = 2, because we don't want either this
  // function itself or its _caller_ to show up in the stack trace.
  return GetStackTrace(StackTraceLineFormat::DEFAULT, /* num_top_frames_to_skip */ 2);
}

// Return the current stack trace, in hex form. This is significantly
// faster than GetStackTrace() above, so should be used in performance-critical
// places like TRACE() calls. If you really need blazing-fast speed, though,
// use HexStackTraceToString() into a stack-allocated buffer instead --
// this call causes a heap allocation for the std::string.
//
// Note that this is much more useful in the context of a static binary,
// since addr2line wouldn't know where shared libraries were mapped at
// runtime.
//
// NOTE: This inherits the same async-safety issue as HexStackTraceToString()
std::string GetStackTraceHex();

// This is the same as GetStackTraceHex(), except multi-line in a format that
// looks very similar to GetStackTrace() but without symbols.
std::string GetLogFormatStackTraceHex();

// Collect the current stack trace in hex form into the given buffer.
//
// The resulting trace just includes the hex addresses, space-separated. This is suitable
// for later stringification by pasting into 'addr2line' for example.
//
// This function is not async-safe, since it uses the libc backtrace() function which
// may invoke the dynamic loader.
void HexStackTraceToString(char* buf, size_t size);

// Active - thread is performing execution.
// Waiting - thread is waiting on mutex.
// Idle - thread is waiting on condition or epoll.
YB_DEFINE_ENUM(StackTraceGroup, (kActive)(kWaiting)(kIdle));

// Efficient class for collecting and later stringifying a stack trace.
//
// Requires external synchronization.
class StackTrace {
 public:
  // It is OK that frames_ is uninitialized.
  //-V:frames_:V730
  StackTrace() : num_frames_(0) {
  }

  void Reset() {
    num_frames_ = 0;
  }

  // Collect and store the current stack trace. Skips the top 'skip_frames' frames
  // from the stack. For example, a value of '1' will skip the 'Collect()' function
  // call itself.
  //
  // This function is technically not async-safe. However, according to
  // http://lists.nongnu.org/archive/html/libunwind-devel/2011-08/msg00054.html it is "largely
  // async safe" and it would only deadlock in the case that you call it while a dynamic library
  // load is in progress. We assume that dynamic library loads would almost always be completed
  // very early in the application lifecycle, so for now, this is considered "async safe" until
  // it proves to be a problem.
  void Collect(int skip_frames = 1);

  enum Flags {
    // Do not fix up the addresses on the stack to try to point to the 'call'
    // instructions instead of the return address. This is necessary when dumping
    // addresses to be interpreted by 'pprof', which does this fix-up itself.
    NO_FIX_CALLER_ADDRESSES = 1
  };

  // Stringify the trace into the given buffer.
  // The resulting output is hex addresses suitable for passing into 'addr2line'
  // later.
  void StringifyToHex(char* buf, size_t size, int flags = 0) const;

  // Same as above, but returning a std::string.
  // This is not async-safe.
  std::string ToHexString(int flags = 0) const;

  // Return a string with a symbolized backtrace in a format suitable for
  // printing to a log file.
  // This is not async-safe.
  // If group is specified it is filled with value corresponding to this stack trace.
  std::string Symbolize(
      StackTraceLineFormat source_file_path_format = StackTraceLineFormat::DEFAULT,
      StackTraceGroup* group = nullptr) const;

  // Return a string with a hex-only backtrace in the format typically used in
  // log files. Similar to the format given by Symbolize(), but symbols are not
  // resolved (only the hex addresses are given).
  std::string ToLogFormatHexString() const;

  uint64_t HashCode() const;

  explicit operator bool() const {
    return num_frames_ != 0;
  }

  bool operator!() const {
    return num_frames_ == 0;
  }

  int compare(const StackTrace& rhs) const {
    return as_slice().compare(rhs.as_slice());
  }

  Slice as_slice() const {
    return Slice(pointer_cast<const char*>(frames_),
                 pointer_cast<const char*>(frames_ + num_frames_));
  }

 private:
  enum {
    // The maximum number of stack frames to collect.
    kMaxFrames = 16,

    // The max number of characters any frame requires in string form.
    kHexEntryLength = 16
  };

  int num_frames_;
  void* frames_[kMaxFrames];

  friend inline bool operator==(const StackTrace& lhs, const StackTrace& rhs) {
    return lhs.num_frames_ == rhs.num_frames_ && lhs.as_slice() == rhs.as_slice();
  }

  friend inline bool operator!=(const StackTrace& lhs, const StackTrace& rhs) {
    return !(lhs == rhs);
  }

  friend inline bool operator<(const StackTrace& lhs, const StackTrace& rhs) {
    return lhs.compare(rhs) < 0;
  }
};

Result<StackTrace> ThreadStack(ThreadIdForStack tid);
// tids should be ordered
std::vector<Result<StackTrace>> ThreadStacks(const std::vector<ThreadIdForStack>& tids);

constexpr bool IsDebug() {
#ifdef NDEBUG
  return false;
#else
  return true;
#endif
}

class ScopeLogger {
 public:
  ScopeLogger(const std::string& msg, std::function<void()> on_scope_bounds)
      : msg_(msg), on_scope_bounds_(std::move(on_scope_bounds)) {
    LOG(INFO) << ">>> " << msg_;
    on_scope_bounds_();
  }

  ~ScopeLogger() {
    on_scope_bounds_();
    LOG(INFO) << "<<< " << msg_;
  }

 private:
  const std::string msg_;
  std::function<void()> on_scope_bounds_;
};

std::string SymbolizeAddress(
    void *pc,
    const StackTraceLineFormat stack_trace_line_format = StackTraceLineFormat::DEFAULT);

// Demangle a C++-mangled identifier.
std::string DemangleName(const char* name);

struct SourceLocation {
  const char* file_name;
  int line_number;

  std::string ToString() const;
};

#define SOURCE_LOCATION() SourceLocation {__FILE__, __LINE__}

#define TEST_PAUSE_IF_FLAG(flag_name) \
    if (PREDICT_FALSE(ANNOTATE_UNPROTECTED_READ(BOOST_PP_CAT(FLAGS_, flag_name)))) { \
      LOG(INFO) << "Pausing due to flag " << #flag_name; \
      do { \
        SleepFor(MonoDelta::FromMilliseconds(100)); \
      } while (ANNOTATE_UNPROTECTED_READ(BOOST_PP_CAT(FLAGS_, flag_name))); \
      LOG(INFO) << "Resuming due to flag " << #flag_name; \
    }

} // namespace yb

#endif  // YB_UTIL_DEBUG_UTIL_H
