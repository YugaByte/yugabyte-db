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

#include <signal.h>

#include <string>
#include <vector>
#include <regex>
#include <sstream>

#include <glog/logging.h>
#include <glog/stl_logging.h>

#include "yb/gutil/ref_counted.h"
#include "yb/util/countdown_latch.h"
#include "yb/util/debug-util.h"
#include "yb/util/scope_exit.h"
#include "yb/util/test_util.h"
#include "yb/util/thread.h"

#include "yb/util/debug/long_operation_tracker.h"

using std::string;
using std::vector;

using namespace std::literals;

namespace yb {

class DebugUtilTest : public YBTest {
 protected:
  void WaitForSleeperThreadNameInStackTrace(ThreadIdForStack thread_id) {
    string stack;
    for (int i = 0; i < 10000; i++) {
      stack = DumpThreadStack(thread_id);
      if (stack.find("SleeperThread") != string::npos) break;
      SleepFor(MonoDelta::FromMicroseconds(100));
    }
    ASSERT_STR_CONTAINS(stack, "SleeperThread");
  }
};

TEST_F(DebugUtilTest, TestStackTrace) {
  StackTrace t;
  t.Collect(1);
  string trace = t.Symbolize();
  LOG(INFO) << "Trace:\n" << trace;
  ASSERT_STR_CONTAINS(trace, "yb::DebugUtilTest_TestStackTrace_Test::TestBody");
  ASSERT_STR_CONTAINS(trace, "testing::internal::UnitTestImpl::RunAllTests()");
  ASSERT_STR_CONTAINS(trace, "main");
}

TEST_F(DebugUtilTest, TestGetStackTrace) {
  string stack_trace = GetStackTrace();

  const std::string kExpectedLineFormatNoFileLineReStr = R"#(^\s*@\s+0x[0-9a-f]+\s+.*)#";
  SCOPED_TRACE(Format("Regex with no file/line: $0", kExpectedLineFormatNoFileLineReStr));
  const std::string kFileLineReStr = R"#( \(\S+:\d+\))#";

  const std::regex kExpectedLineFormatNoFileLineRe(kExpectedLineFormatNoFileLineReStr + "$");
  const std::regex kExpectedLineFormatWithFileLineRe(
      kExpectedLineFormatNoFileLineReStr + kFileLineReStr + "$");
  const std::regex kNilUnknownRe(R"#(^\s*@\s+\(nil\)\s+\(unknown\)$)#");

  // Expected line format:
  // @ 0x41255d yb::DebugUtilTest_TestGetStackTrace_Test::TestBody() (yb/util/debug-util-test.cc:73)
  SCOPED_TRACE(Format("Stack trace to be checked:\n:$0", stack_trace));
  std::stringstream ss(stack_trace);
  std::smatch match;

  int with_file_line = 0;
  int without_file_line = 0;
  int unmatched = 0;
  int num_lines = 0;
  std::ostringstream debug_info;
  std::string next_line;
  std::getline(ss, next_line);
  while (ss) {
    const auto line = next_line;
    std::getline(ss, next_line);
    if (std::regex_match(line, match, kExpectedLineFormatWithFileLineRe)) {
      ++with_file_line;
      debug_info << "Line matched regex with file/line number: " << line << std::endl;
    } else if (std::regex_match(line, match, kExpectedLineFormatNoFileLineRe)) {
      ++without_file_line;
      debug_info << "Line matched regex without file/line number: " << line << std::endl;
    } else if (!ss && std::regex_match(line, match, kNilUnknownRe)) {
      debug_info << "Last line matched '(nil) (unknown)' pattern: " << line << std::endl;
    } else {
      ++unmatched;
      debug_info << "Line did not match either regex: " << line << std::endl;
    }
    ++num_lines;
  }
  SCOPED_TRACE(debug_info.str());
  ASSERT_EQ(unmatched, 0);
  ASSERT_GE(num_lines, 0);
#if defined(__linux__) && !defined(NDEBUG)
  ASSERT_GE(with_file_line, 0);
#else
  ASSERT_GE(without_file_line, 0);
#endif
}

// DumpThreadStack is only supported on Linux, since the implementation relies
// on the tgkill syscall which is not portable.
//
// TODO: it might be possible to enable other tests in this section to work on macOS.

TEST_F(DebugUtilTest, TestStackTraceInvalidTid) {
#if defined(__linux__)
  ThreadIdForStack bad_tid = 1;
#else
  ThreadIdForStack bad_tid = reinterpret_cast<ThreadIdForStack>(1);
#endif
  string s = DumpThreadStack(bad_tid);
  ASSERT_STR_CONTAINS(s, "Unable to deliver signal");
}

TEST_F(DebugUtilTest, TestStackTraceSelf) {
  string s = DumpThreadStack(Thread::CurrentThreadIdForStack());
  ASSERT_STR_CONTAINS(s, "yb::DebugUtilTest_TestStackTraceSelf_Test::TestBody()");
}

#if defined(__linux__)

TEST_F(DebugUtilTest, TestStackTraceMainThread) {
  string s = DumpThreadStack(getpid());
  ASSERT_STR_CONTAINS(s, "yb::DebugUtilTest_TestStackTraceMainThread_Test::TestBody()");
}

#endif

namespace {

void SleeperThread(CountDownLatch* l) {
  // We use an infinite loop around WaitFor() instead of a normal Wait()
  // so that this test passes in TSAN. Without this, we run into this TSAN
  // bug which prevents the sleeping thread from handling signals:
  // https://code.google.com/p/thread-sanitizer/issues/detail?id=91
  while (!l->WaitFor(MonoDelta::FromMilliseconds(10))) {
  }
}

void fake_signal_handler(int signum) {}

bool IsSignalHandlerRegistered(int signum) {
  struct sigaction cur_action;
  CHECK_EQ(0, sigaction(signum, nullptr, &cur_action));
  return cur_action.sa_handler != SIG_DFL;
}

} // anonymous namespace

TEST_F(DebugUtilTest, TestSignalStackTrace) {
  CountDownLatch l(1);
  scoped_refptr<Thread> t;
  ASSERT_OK(Thread::Create("test", "test thread", &SleeperThread, &l, &t));

  // We have to loop a little bit because it takes a little while for the thread
  // to start up and actually call our function.
  WaitForSleeperThreadNameInStackTrace(t->tid_for_stack());

  // Test that we can change the signal and that the stack traces still work,
  // on the new signal.
  ASSERT_FALSE(IsSignalHandlerRegistered(SIGUSR1));
  ASSERT_OK(SetStackTraceSignal(SIGUSR1));

  // Should now be registered.
  ASSERT_TRUE(IsSignalHandlerRegistered(SIGUSR1));

  // SIGUSR2 should be relinquished.
  ASSERT_FALSE(IsSignalHandlerRegistered(SIGUSR2));

  // Stack traces should work using the new handler. We've had a test failure here when we ust had
  // a one-time check, so we do the same waiting loop as in the beginning of the test.
  WaitForSleeperThreadNameInStackTrace(t->tid_for_stack());

  // Switch back to SIGUSR2 and ensure it changes back.
  ASSERT_OK(SetStackTraceSignal(SIGUSR2));
  ASSERT_TRUE(IsSignalHandlerRegistered(SIGUSR2));
  ASSERT_FALSE(IsSignalHandlerRegistered(SIGUSR1));

  // Stack traces should work using the new handler. Also has a test failure here, so using a retry
  // loop.
  WaitForSleeperThreadNameInStackTrace(t->tid_for_stack());

  // Register our own signal handler on SIGUSR1, and ensure that
  // we get a bad Status if we try to use it.
  signal(SIGUSR1, &fake_signal_handler);
  ASSERT_STR_CONTAINS(SetStackTraceSignal(SIGUSR1).ToString(),
                      "Unable to install signal handler");
  signal(SIGUSR1, SIG_IGN);

  // Stack traces should be disabled
  ASSERT_STR_CONTAINS(DumpThreadStack(t->tid_for_stack()), "Unable to take thread stack");

  // Re-enable so that other tests pass.
  ASSERT_OK(SetStackTraceSignal(SIGUSR2));

  // Allow the thread to finish.
  l.CountDown();
  t->Join();
}

#if defined(__linux__)

// Test which dumps all known threads within this process.
// We don't validate the results in any way -- but this verifies that we can
// dump library threads such as the libc timer_thread and properly time out.
TEST_F(DebugUtilTest, TestDumpAllThreads) {
  std::vector<pid_t> tids;
  ASSERT_OK(ListThreads(&tids));
  for (pid_t tid : tids) {
    LOG(INFO) << DumpThreadStack(tid);
  }
}

#endif

// This will probably be really slow on Mac OS X, so only enabling on Linux.
TEST_F(DebugUtilTest, TestGetStackTraceInALoop) {
  for (int i = 1; i <= 10000; ++i) {
    GetStackTrace();
  }
}

TEST_F(DebugUtilTest, TestConcurrentStackTrace) {
  constexpr size_t kTotalThreads = 10;
  constexpr size_t kNumCycles = 100;
  std::vector<std::thread> threads;
  LOG(INFO) << "Spawning threads";
  while (threads.size() != kTotalThreads) {
    threads.emplace_back([] {
      LOG(INFO) << "Thread started";
      for (size_t i = 0; i != kNumCycles; ++i) {
        GetStackTrace();
      }
      LOG(INFO) << "Thread finished";
    });
  }

  LOG(INFO) << "Joining thread";
  for (auto& thread : threads) {
    thread.join();
  }
}

TEST_F(DebugUtilTest, LongOperationTracker) {
  class TestLogSink : public google::LogSink {
   public:
    void send(google::LogSeverity severity, const char* full_filename,
              const char* base_filename, int line,
              const struct ::tm* tm_time,
              const char* message, size_t message_len) override {
      std::lock_guard<std::mutex> lock(mutex_);
      log_messages_.emplace_back(message, message_len);
    }

    size_t MessagesSize() {
      std::lock_guard<std::mutex> lock(mutex_);
      return log_messages_.size();
    }

    std::string MessageAt(size_t idx) {
      std::lock_guard<std::mutex> lock(mutex_);
      return log_messages_[idx];
    }

   private:
    std::mutex mutex_;
    std::vector<std::string> log_messages_;
  };

#ifndef NDEBUG
  const auto kTimeMultiplier = RegularBuildVsSanitizers(3, 10);
#else
  const auto kTimeMultiplier = 1;
#endif

  const auto kShortDuration = 100ms * kTimeMultiplier;
  const auto kMidDuration = 400ms * kTimeMultiplier;
  const auto kLongDuration = 1000ms * kTimeMultiplier;
  TestLogSink log_sink;
  google::AddLogSink(&log_sink);
  auto se = ScopeExit([&log_sink] {
    google::RemoveLogSink(&log_sink);
  });

  {
    LongOperationTracker tracker("Op1", kLongDuration);
    std::this_thread::sleep_for(kShortDuration);
  }
  {
    LongOperationTracker tracker("Op2", kShortDuration);
    std::this_thread::sleep_for(kLongDuration);
  }
  {
    LongOperationTracker tracker1("Op3", kLongDuration);
    LongOperationTracker tracker2("Op4", kShortDuration);
    std::this_thread::sleep_for(kMidDuration);
  }

  std::this_thread::sleep_for(kLongDuration);

  ASSERT_EQ(log_sink.MessagesSize(), 4);
  ASSERT_STR_CONTAINS(log_sink.MessageAt(0), "Op2");
  ASSERT_STR_CONTAINS(log_sink.MessageAt(1), "Op2");
  ASSERT_STR_CONTAINS(log_sink.MessageAt(2), "Op4");
  ASSERT_STR_CONTAINS(log_sink.MessageAt(3), "Op4");
}

} // namespace yb
