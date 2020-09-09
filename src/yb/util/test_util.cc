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
#include "yb/util/test_util.h"

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest-spi.h>

#include "yb/gutil/strings/strcat.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/strings/util.h"
#include "yb/gutil/walltime.h"
#include "yb/util/env.h"
#include "yb/util/path_util.h"
#include "yb/util/random.h"
#include "yb/util/spinlock_profiling.h"
#include "yb/util/thread.h"
#include "yb/util/logging.h"

DEFINE_string(test_leave_files, "on_failure",
              "Whether to leave test files around after the test run. "
              " Valid values are 'always', 'on_failure', or 'never'");

DEFINE_int32(test_random_seed, 0, "Random seed to use for randomized tests");
DECLARE_int64(memory_limit_hard_bytes);
DECLARE_bool(enable_tracing);
DECLARE_bool(TEST_running_test);

using std::string;
using strings::Substitute;
using gflags::FlagSaver;

namespace yb {

static const char* const kSlowTestsEnvVariable = "YB_ALLOW_SLOW_TESTS";

static const uint64 kTestBeganAtMicros = Env::Default()->NowMicros();

///////////////////////////////////////////////////
// YBTest
///////////////////////////////////////////////////

YBTest::YBTest()
  : env_(new EnvWrapper(Env::Default())),
    test_dir_(GetTestDataDirectory()) {
  InitThreading();
}

// env passed in from subclass, for tests that run in-memory
YBTest::YBTest(Env *env)
  : env_(env),
    test_dir_(GetTestDataDirectory()) {
}

YBTest::~YBTest() {
  // Clean up the test directory in the destructor instead of a TearDown
  // method. This is better because it ensures that the child-class
  // dtor runs first -- so, if the child class is using a minicluster, etc,
  // we will shut that down before we remove files underneath.
  if (FLAGS_test_leave_files == "always") {
    LOG(INFO) << "-----------------------------------------------";
    LOG(INFO) << "--test_leave_files specified, leaving files in " << test_dir_;
  } else if (FLAGS_test_leave_files == "on_failure" && HasFatalFailure()) {
    LOG(INFO) << "-----------------------------------------------";
    LOG(INFO) << "Had fatal failures, leaving test files at " << test_dir_;
  } else {
    VLOG(1) << "Cleaning up temporary test files...";
    WARN_NOT_OK(env_->DeleteRecursively(test_dir_),
                "Couldn't remove test files");
  }
}

void YBTest::SetUp() {
  InitSpinLockContentionProfiling();
  InitGoogleLoggingSafeBasic("yb_test");
  FLAGS_enable_tracing = true;
  FLAGS_memory_limit_hard_bytes = 8 * 1024 * 1024 * 1024L;
  FLAGS_TEST_running_test = true;
  for (const char* env_var_name : {
      "ASAN_OPTIONS",
      "LSAN_OPTIONS",
      "UBSAN_OPTIONS",
      "TSAN_OPTIONS"
  }) {
    const char* value = getenv(env_var_name);
    if (value && value[0]) {
      LOG(INFO) << "Environment variable " << env_var_name << ": " << value;
    }
  }
}

string YBTest::GetTestPath(const string& relative_path) {
  CHECK(!test_dir_.empty()) << "Call SetUp() first";
  return JoinPathSegments(test_dir_, relative_path);
}

///////////////////////////////////////////////////
// Test utility functions
///////////////////////////////////////////////////

bool AllowSlowTests() {
  char *e = getenv(kSlowTestsEnvVariable);
  if ((e == nullptr) ||
      (strlen(e) == 0) ||
      (strcasecmp(e, "false") == 0) ||
      (strcasecmp(e, "0") == 0) ||
      (strcasecmp(e, "no") == 0)) {
    return false;
  }
  if ((strcasecmp(e, "true") == 0) ||
      (strcasecmp(e, "1") == 0) ||
      (strcasecmp(e, "yes") == 0)) {
    return true;
  }
  LOG(FATAL) << "Unrecognized value for " << kSlowTestsEnvVariable << ": " << e;
  return false;
}

void OverrideFlagForSlowTests(const std::string& flag_name,
                              const std::string& new_value) {
  // Ensure that the flag is valid.
  google::GetCommandLineFlagInfoOrDie(flag_name.c_str());

  // If we're not running slow tests, don't override it.
  if (!AllowSlowTests()) {
    return;
  }
  google::SetCommandLineOptionWithMode(flag_name.c_str(), new_value.c_str(),
                                       google::SET_FLAG_IF_DEFAULT);
}

int SeedRandom() {
  int seed;
  // Initialize random seed
  if (FLAGS_test_random_seed == 0) {
    // Not specified by user
    seed = static_cast<int>(GetCurrentTimeMicros());
  } else {
    seed = FLAGS_test_random_seed;
  }
  LOG(INFO) << "Using random seed: " << seed;
  srand(seed);
  return seed;
}

string GetTestDataDirectory() {
  const ::testing::TestInfo* const test_info =
    ::testing::UnitTest::GetInstance()->current_test_info();
  CHECK(test_info) << "Must be running in a gtest unit test to call this function";
  string dir;
  CHECK_OK(Env::Default()->GetTestDirectory(&dir));

  // The directory name includes some strings for specific reasons:
  // - program name: identifies the directory to the test invoker
  // - timestamp and pid: disambiguates with prior runs of the same test
  //
  // e.g. "env-test.TestEnv.TestReadFully.1409169025392361-23600"
  dir += Substitute("/$0.$1.$2.$3-$4",
    StringReplace(google::ProgramInvocationShortName(), "/", "_", true),
    StringReplace(test_info->test_case_name(), "/", "_", true),
    StringReplace(test_info->name(), "/", "_", true),
    kTestBeganAtMicros,
    getpid());
  Status s = Env::Default()->CreateDir(dir);
  CHECK(s.IsAlreadyPresent() || s.ok())
    << "Could not create directory " << dir << ": " << s.ToString();
  if (s.ok()) {
    string metadata;

    StrAppend(&metadata, Substitute("PID=$0\n", getpid()));

    StrAppend(&metadata, Substitute("PPID=$0\n", getppid()));

    char* jenkins_build_id = getenv("BUILD_ID");
    if (jenkins_build_id) {
      StrAppend(&metadata, Substitute("BUILD_ID=$0\n", jenkins_build_id));
    }

    CHECK_OK(WriteStringToFile(Env::Default(), metadata,
                               Substitute("$0/test_metadata", dir)));
  }
  return dir;
}

void AssertEventually(const std::function<void(void)>& f,
                      const MonoDelta& timeout) {
  const MonoTime deadline = MonoTime::Now() + timeout;
  {
    FlagSaver flag_saver;
    // Disable --gtest_break_on_failure, or else the assertion failures
    // inside our attempts will cause the test to SEGV even though we
    // would like to retry.
    testing::FLAGS_gtest_break_on_failure = false;

    for (int attempts = 0; MonoTime::Now() < deadline; attempts++) {
      // Capture any assertion failures within this scope (i.e. from their function)
      // into 'results'
      testing::TestPartResultArray results;
      testing::ScopedFakeTestPartResultReporter reporter(
          testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD,
          &results);
      f();

      // Determine whether their function produced any new test failure results.
      bool has_failures = false;
      for (int i = 0; i < results.size(); i++) {
        has_failures |= results.GetTestPartResult(i).failed();
      }
      if (!has_failures) {
        return;
      }

      // If they had failures, sleep and try again.
      int sleep_ms = (attempts < 10) ? (1 << attempts) : 1000;
      SleepFor(MonoDelta::FromMilliseconds(sleep_ms));
    }
  }

  // If we ran out of time looping, run their function one more time
  // without capturing its assertions. This way the assertions will
  // propagate back out to the normal test reporter. Of course it's
  // possible that it will pass on this last attempt, but that's OK
  // too, since we aren't trying to be that strict about the deadline.
  f();
  if (testing::Test::HasFatalFailure()) {
    ADD_FAILURE() << "Timed out waiting for assertion to pass.";
  }
}

Status Wait(std::function<Result<bool>()> condition,
            MonoTime deadline,
            const std::string& description,
            MonoDelta initial_delay,
            double delay_multiplier,
            MonoDelta max_delay) {
  auto start = MonoTime::Now();
  MonoDelta delay = initial_delay;
  for (;;) {
    const auto current = condition();
    if (!current.ok()) {
      return current.status();
    }
    if (current.get()) {
      break;
    }
    const auto now = MonoTime::Now();
    const auto left = deadline - now;
    if (left <= MonoDelta::kZero) {
      return STATUS_FORMAT(TimedOut,
                           "Operation '$0' didn't complete within $1ms",
                           description,
                           (now - start).ToMilliseconds());
    }
    delay = std::min(std::min(MonoDelta::FromSeconds(delay.ToSeconds() * delay_multiplier), left),
                     max_delay);
    SleepFor(delay);
  }
  return Status::OK();
}

// Waits for the given condition to be true or until the provided timeout has expired.
Status WaitFor(std::function<Result<bool>()> condition,
               MonoDelta timeout,
               const string& description,
               MonoDelta initial_delay,
               double delay_multiplier,
               MonoDelta max_delay) {
  return Wait(condition, MonoTime::Now() + timeout, description, initial_delay, delay_multiplier,
              max_delay);
}

void AssertLoggedWaitFor(
    std::function<Result<bool>()> condition,
    MonoDelta timeout,
    const string& description,
    MonoDelta initial_delay,
    double delay_multiplier,
    MonoDelta max_delay) {
  LOG(INFO) << description;
  ASSERT_OK(WaitFor(condition, timeout, description, initial_delay));
  LOG(INFO) << description << " - DONE";
}

Status LoggedWaitFor(
    std::function<Result<bool>()> condition,
    MonoDelta timeout,
    const string& description,
    MonoDelta initial_delay,
    double delay_multiplier,
    MonoDelta max_delay) {
  LOG(INFO) << description << " - started";
  auto status = WaitFor(condition, timeout, description, initial_delay);
  LOG(INFO) << description << " - completed: " << yb::ToString(status);
  return status;
}

string GetToolPath(const string& rel_path, const string& tool_name) {
  string exe;
  CHECK_OK(Env::Default()->GetExecutablePath(&exe));
  const string binroot = JoinPathSegments(DirName(exe), rel_path);
  const string tool_path = JoinPathSegments(binroot, tool_name);
  CHECK(Env::Default()->FileExists(tool_path)) << tool_name << " tool not found at " << tool_path;
  return tool_path;
}

int CalcNumTablets(int num_tablet_servers) {
#ifdef NDEBUG
  return 0;  // Will use the default.
#elif defined(THREAD_SANITIZER) || defined(ADDRESS_SANITIZER)
  return num_tablet_servers;
#else
  return num_tablet_servers * 3;
#endif
}

void WaitStopped(const CoarseDuration& duration, std::atomic<bool>* stop) {
  auto end = CoarseMonoClock::now() + duration;
  while (!stop->load(std::memory_order_acquire) && CoarseMonoClock::now() < end) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void TestThreadHolder::JoinAll() {
  LOG(INFO) << __func__;

  for (auto& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }

  LOG(INFO) << __func__ << " done";
}

} // namespace yb
