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
// Base test class, with various utility functions.
#ifndef YB_UTIL_TEST_UTIL_H
#define YB_UTIL_TEST_UTIL_H

#include <atomic>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "yb/gutil/gscoped_ptr.h"

#include "yb/util/env.h"
#include "yb/util/monotime.h"
#include "yb/util/result.h"
#include "yb/util/port_picker.h"
#include "yb/util/test_macros.h"
#include "yb/util/thread.h"
#include "yb/util/tsan_util.h"

#define ASSERT_EVENTUALLY(expr) do { \
  AssertEventually(expr); \
  NO_PENDING_FATALS(); \
} while (0)

namespace yb {
namespace rpc {

class Messenger;

} // namespace rpc

// Our test string literals contain "\x00" that is treated as a C-string null-terminator.
// So we need to call the std::string constructor that takes the length argument.
#define BINARY_STRING(s) string((s), sizeof(s) - 1)

class YBTest : public ::testing::Test {
 public:
  YBTest();

  // Env passed in from subclass, for tests that run in-memory.
  explicit YBTest(Env *env);

  virtual ~YBTest();

  virtual void SetUp() override;

 protected:
  // Returns absolute path based on a unit test-specific work directory, given
  // a relative path. Useful for writing test files that should be deleted after
  // the test ends.
  std::string GetTestPath(const std::string& relative_path);

  uint16_t AllocateFreePort() { return port_picker_.AllocateFreePort(); }

  gscoped_ptr<Env> env_;
  google::FlagSaver flag_saver_;  // Reset flags on every test.
  PortPicker port_picker_;

 private:
  std::string test_dir_;
};

// Returns true if slow tests are runtime-enabled.
bool AllowSlowTests();

// Override the given gflag to the new value, only in the case that
// slow tests are enabled and the user hasn't otherwise overridden
// it on the command line.
// Example usage:
//
// OverrideFlagForSlowTests(
//     "client_inserts_per_thread",
//     strings::Substitute("$0", FLAGS_client_inserts_per_thread * 100));
//
void OverrideFlagForSlowTests(const std::string& flag_name,
                              const std::string& new_value);

// Call srand() with a random seed based on the current time, reporting
// that seed to the logs. The time-based seed may be overridden by passing
// --test_random_seed= from the CLI in order to reproduce a failed randomized
// test. Returns the seed.
int SeedRandom();

// Return a per-test directory in which to store test data. Guaranteed to
// return the same directory every time for a given unit test.
//
// May only be called from within a gtest unit test.
std::string GetTestDataDirectory();

// Wait until 'f()' succeeds without adding any GTest 'fatal failures'.
// For example:
//
//   AssertEventually([]() {
//     ASSERT_GT(ReadValueOfMetric(), 10);
//   });
//
// The function is run in a loop with exponential backoff, capped at once
// a second.
//
// To check whether AssertEventually() eventually succeeded, call
// NO_PENDING_FATALS() afterward, or use ASSERT_EVENTUALLY() which performs
// this check automatically.
void AssertEventually(const std::function<void(void)>& f,
                      const MonoDelta& timeout = MonoDelta::FromSeconds(30));

// Logs some of the differences between the two given vectors. This can be used immediately before
// asserting that two vectors are equal to make debugging easier.
template<typename T>
void LogVectorDiff(const std::vector<T>& expected, const std::vector<T>& actual) {
  if (expected.size() != actual.size()) {
    LOG(WARNING) << "Expected size: " << expected.size() << ", actual size: " << actual.size();
    const std::vector<T> *bigger_vector, *smaller_vector;
    const char *bigger_vector_desc;
    if (expected.size() > actual.size()) {
      bigger_vector = &expected;
      bigger_vector_desc = "expected";
      smaller_vector = &actual;
    } else {
      bigger_vector = &actual;
      bigger_vector_desc = "actual";
      smaller_vector = &expected;
    }

    for (int i = smaller_vector->size();
         i < min(smaller_vector->size() + 16, bigger_vector->size());
         ++i) {
      LOG(WARNING) << bigger_vector_desc << "[" << i << "]: " << (*bigger_vector)[i];
    }
  }
  int num_differences_logged = 0;
  size_t num_differences_left = 0;
  size_t min_size = min(expected.size(), actual.size());
  for (size_t i = 0; i < min_size; ++i) {
    if (expected[i] != actual[i]) {
      if (num_differences_logged < 16) {
        LOG(WARNING) << "expected[" << i << "]: " << expected[i];
        LOG(WARNING) << "actual  [" << i << "]: " << actual[i];
        ++num_differences_logged;
      } else {
        ++num_differences_left;
      }
    }
  }
  if (num_differences_left > 0) {
    if (expected.size() == actual.size()) {
      LOG(WARNING) << num_differences_left << " more differences omitted";
    } else {
      LOG(WARNING) << num_differences_left << " more differences in the first " << min_size
      << " elements omitted";
    }
  }
}

namespace test_util {

constexpr int kDefaultInitialWaitMs = 1;
constexpr double kDefaultWaitDelayMultiplier = 1.1;
constexpr int kDefaultMaxWaitDelayMs = 2000;

} // namespace test_util

// Waits for the given condition to be true or until the provided deadline happens.
CHECKED_STATUS Wait(
    std::function<Result<bool>()> condition,
    MonoTime deadline,
    const std::string& description,
    MonoDelta initial_delay = MonoDelta::FromMilliseconds(test_util::kDefaultInitialWaitMs),
    double delay_multiplier = test_util::kDefaultWaitDelayMultiplier,
    MonoDelta max_delay = MonoDelta::FromMilliseconds(test_util::kDefaultMaxWaitDelayMs));

// Waits for the given condition to be true or until the provided timeout has expired.
CHECKED_STATUS WaitFor(
    std::function<Result<bool>()> condition,
    MonoDelta timeout,
    const std::string& description,
    MonoDelta initial_delay = MonoDelta::FromMilliseconds(test_util::kDefaultInitialWaitMs),
    double delay_multiplier = test_util::kDefaultWaitDelayMultiplier,
    MonoDelta max_delay = MonoDelta::FromMilliseconds(test_util::kDefaultMaxWaitDelayMs));

void AssertLoggedWaitFor(
    std::function<Result<bool>()> condition,
    MonoDelta timeout,
    const string& description,
    MonoDelta initial_delay = MonoDelta::FromMilliseconds(test_util::kDefaultInitialWaitMs),
    double delay_multiplier = test_util::kDefaultWaitDelayMultiplier,
    MonoDelta max_delay = MonoDelta::FromMilliseconds(test_util::kDefaultMaxWaitDelayMs));

CHECKED_STATUS LoggedWaitFor(
    std::function<Result<bool>()> condition,
    MonoDelta timeout,
    const string& description,
    MonoDelta initial_delay = MonoDelta::FromMilliseconds(test_util::kDefaultInitialWaitMs),
    double delay_multiplier = test_util::kDefaultWaitDelayMultiplier,
    MonoDelta max_delay = MonoDelta::FromMilliseconds(test_util::kDefaultMaxWaitDelayMs));

// Return the path of a yb-tool.
std::string GetToolPath(const std::string& rel_path, const std::string& tool_name);

inline std::string GetToolPath(const std::string& tool_name) {
  return GetToolPath("../bin", tool_name);
}

inline std::string GetPgToolPath(const std::string& tool_name) {
  return GetToolPath("../postgres/bin", tool_name);
}

int CalcNumTablets(int num_tablet_servers);

class StopOnFailure {
 public:
  explicit StopOnFailure(std::atomic<bool>* stop) : stop_(*stop) {}

  StopOnFailure(const StopOnFailure&) = delete;
  void operator=(const StopOnFailure&) = delete;

  ~StopOnFailure() {
    if (!success_) {
      stop_.store(true, std::memory_order_release);
    }
  }

  void Success() {
    success_ = true;
  }
 private:
  bool success_ = false;
  std::atomic<bool>& stop_;
};

// Waits specified duration or when stop switches to true.
void WaitStopped(const CoarseDuration& duration, std::atomic<bool>* stop);

class SetFlagOnExit {
 public:
  explicit SetFlagOnExit(std::atomic<bool>* stop_flag)
      : stop_flag_(stop_flag) {}

  ~SetFlagOnExit() {
    stop_flag_->store(true, std::memory_order_release);
  }

 private:
  std::atomic<bool>* stop_flag_;
};

// Holds vector of threads, and provides convenient utilities. Such as JoinAll, Wait etc.
class TestThreadHolder {
 public:
  ~TestThreadHolder() {
    stop_flag_.store(true, std::memory_order_release);
    JoinAll();
  }

  template <class... Args>
  void AddThread(Args&&... args) {
    threads_.emplace_back(std::forward<Args>(args)...);
  }

  void AddThread(std::thread thread) {
    threads_.push_back(std::move(thread));
  }

  template <class Functor>
  void AddThreadFunctor(const Functor& functor) {
    AddThread([&stop = stop_flag_, functor] {
      CDSAttacher attacher;
      SetFlagOnExit set_stop_on_exit(&stop);
      functor();
    });
  }

  void Wait(const CoarseDuration& duration) {
    yb::WaitStopped(duration, &stop_flag_);
  }

  void JoinAll();

  template <class Cond>
  CHECKED_STATUS WaitCondition(const Cond& cond) {
    while (!cond()) {
      if (stop_flag_.load(std::memory_order_acquire)) {
        return STATUS(Aborted, "Wait aborted");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return Status::OK();
  }

  void WaitAndStop(const CoarseDuration& duration) {
    yb::WaitStopped(duration, &stop_flag_);
    Stop();
  }

  void Stop() {
    stop_flag_.store(true, std::memory_order_release);
    JoinAll();
  }

  std::atomic<bool>& stop_flag() {
    return stop_flag_;
  }

 private:
  std::atomic<bool> stop_flag_{false};
  std::vector<std::thread> threads_;
};

} // namespace yb

// Gives ability to define custom parent class for test fixture.
#define TEST_F_EX(test_case_name, test_name, parent_class) \
  GTEST_TEST_(test_case_name, test_name, parent_class, \
              ::testing::internal::GetTypeId<test_case_name>())

#endif  // YB_UTIL_TEST_UTIL_H
