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

#include "yb/util/monotime.h"

#include <sys/time.h>
#include <unistd.h>

#include <condition_variable>
#include <thread>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "yb/util/test_util.h"

using namespace std::literals;

namespace yb {

TEST(TestMonoTime, TestMonotonicity) {
  alarm(360);
  MonoTime prev(MonoTime::Now());
  MonoTime next;

  do {
    next = MonoTime::Now();
  } while (!prev.ComesBefore(next));
  ASSERT_FALSE(next.ComesBefore(prev));
  alarm(0);
}

TEST(TestMonoTime, TestComparison) {
  auto now = CoarseMonoClock::Now();
  auto future = now + 1ns;

  ASSERT_GT(ToNanoseconds(future - now), 0);
  ASSERT_LT(ToNanoseconds(now - future), 0);
  ASSERT_EQ(ToNanoseconds(now - now), 0);

  ASSERT_LT(now, future);
  ASSERT_FALSE(now < now);
  ASSERT_FALSE(future < now);
  ASSERT_LE(now, now);
  ASSERT_LE(now, future);
  ASSERT_FALSE(future <= now);
  ASSERT_GT(future, now);
  ASSERT_FALSE(now > now);
  ASSERT_FALSE(now > future);
  ASSERT_GE(now, now);
  ASSERT_GE(future, now);
  ASSERT_FALSE(now >= future);

  MonoDelta nano(MonoDelta::FromNanoseconds(1L));
  MonoDelta mil(MonoDelta::FromMilliseconds(1L));
  MonoDelta sec(MonoDelta::FromSeconds(1.0));

  ASSERT_TRUE(nano.LessThan(mil));
  ASSERT_TRUE(mil.LessThan(sec));
  ASSERT_TRUE(mil.MoreThan(nano));
  ASSERT_TRUE(sec.MoreThan(mil));
}

TEST(TestMonoTime, TestTimeVal) {
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 0;

  // Normal conversion case.
  MonoDelta one_sec_one_micro(MonoDelta::FromNanoseconds(1000001000L));
  one_sec_one_micro.ToTimeVal(&tv);
  ASSERT_EQ(1, tv.tv_sec);
  ASSERT_EQ(1, tv.tv_usec);

  // Case where we are still positive but sub-micro.
  // Round up to nearest microsecond. This is to avoid infinite timeouts
  // in APIs that take a struct timeval.
  MonoDelta zero_sec_one_nano(MonoDelta::FromNanoseconds(1L));
  zero_sec_one_nano.ToTimeVal(&tv);
  ASSERT_EQ(0, tv.tv_sec);
  ASSERT_EQ(1, tv.tv_usec); // Special case: 1ns rounds up to

  // Negative conversion case. Ensure the timeval is normalized.
  // That means sec is negative and usec is positive.
  MonoDelta neg_micro(MonoDelta::FromMicroseconds(-1L));
  ASSERT_EQ(-1000, neg_micro.ToNanoseconds());
  neg_micro.ToTimeVal(&tv);
  ASSERT_EQ(-1, tv.tv_sec);
  ASSERT_EQ(999999, tv.tv_usec);

  // Case where we are still negative but sub-micro.
  // Round up to nearest microsecond. This is to avoid infinite timeouts
  // in APIs that take a struct timeval and for consistency.
  MonoDelta zero_sec_neg_one_nano(MonoDelta::FromNanoseconds(-1L));
  zero_sec_neg_one_nano.ToTimeVal(&tv);
  ASSERT_EQ(-1, tv.tv_sec);
  ASSERT_EQ(999999, tv.tv_usec);
}

TEST(TestMonoTime, TestTimeSpec) {
  timespec ts;

  MonoDelta::FromNanoseconds(2L).ToTimeSpec(&ts);
  ASSERT_EQ(0, ts.tv_sec);
  ASSERT_EQ(2, ts.tv_nsec);

  // Negative conversion case. Ensure the timespec is normalized.
  // That means sec is negative and nsec is positive.
  MonoDelta neg_nano(MonoDelta::FromNanoseconds(-1L));
  ASSERT_EQ(-1, neg_nano.ToNanoseconds());
  neg_nano.ToTimeSpec(&ts);
  ASSERT_EQ(-1, ts.tv_sec);
  ASSERT_EQ(999999999, ts.tv_nsec);

}

TEST(TestMonoTime, TestDeltas) {
  alarm(360);
  const MonoDelta max_delta(MonoDelta::FromSeconds(0.1));
  MonoTime prev(MonoTime::Now());
  MonoTime next;
  MonoDelta cur_delta;
  do {
    next = MonoTime::Now();
    cur_delta = next.GetDeltaSince(prev);
  } while (cur_delta.LessThan(max_delta));
  alarm(0);
}

TEST(TestMonoTime, TestDeltaConversions) {
  // TODO: Reliably test MonoDelta::FromSeconds() considering floating-point rounding errors

  MonoDelta mil(MonoDelta::FromMilliseconds(500));
  ASSERT_EQ(500 * MonoTime::kNanosecondsPerMillisecond, mil.nano_delta_);

  MonoDelta micro(MonoDelta::FromMicroseconds(500));
  ASSERT_EQ(500 * MonoTime::kNanosecondsPerMicrosecond, micro.nano_delta_);

  MonoDelta nano(MonoDelta::FromNanoseconds(500));
  ASSERT_EQ(500, nano.nano_delta_);
}

template <class Now>
static void DoTestMonoTimePerf(const std::string& name, Now now) {
  uint64_t num_calls = 0;
  auto start = now();
  auto stop = start + 500ms;
  do {
    num_calls++;
  } while (now() < stop);
  LOG(INFO) << "DoTestMonoTimePerf(" << name << "): " << num_calls << " in "
        << ToSeconds(now() - start) << " seconds.";
}

TEST(TestMonoTime, TestSleepFor) {
  MonoTime start = MonoTime::Now();
  MonoDelta sleep = MonoDelta::FromMilliseconds(100);
  SleepFor(sleep);
  MonoTime end = MonoTime::Now();
  MonoDelta actualSleep = end.GetDeltaSince(start);
  ASSERT_GE(actualSleep.ToNanoseconds(), sleep.ToNanoseconds());
}

TEST(TestMonoTime, TestSleepForOverflow) {
  if (!AllowSlowTests()) {
    LOG(INFO) << "Skipping test because it sleeps for ~4s";
    return;
  }

  // This quantity (~4s sleep) overflows a 32-bit integer such that
  // the value becomes 0.
  MonoTime start = MonoTime::Now();
  MonoDelta sleep = MonoDelta::FromNanoseconds(1L << 32);
  SleepFor(sleep);
  MonoTime end = MonoTime::Now();
  MonoDelta actualSleep = end.GetDeltaSince(start);
  ASSERT_GE(actualSleep.ToNanoseconds(), sleep.ToNanoseconds());
}

TEST(TestMonoTimePerf, TestMonoTimePerfCoarse) {
  alarm(360);
  DoTestMonoTimePerf("CoarseMonoClock", [] { return CoarseMonoClock::Now(); });
  alarm(0);
}

TEST(TestMonoTimePerf, TestMonoTimePerfFine) {
  alarm(360);
  DoTestMonoTimePerf("MonoTime", [] { return MonoTime::Now(); });
  alarm(0);
}

TEST(TestMonoTime, ToCoarse) {
  for (int i = 0; i != 1000; ++i) {
    auto before = CoarseMonoClock::Now();
    auto converted = ToCoarse(MonoTime::Now());
    auto after = CoarseMonoClock::Now();

    // Coarse mono clock has 1ms precision, so we add it to bounds.
    const auto kPrecision = 2ms;
    ASSERT_GE(converted, before);
    ASSERT_LE(converted, after + kPrecision);
  }
}

TEST(TestMonoTime, TestOverFlow) {
  EXPECT_EXIT(MonoDelta::FromMilliseconds(std::numeric_limits<int64_t>::max()),
                                          ::testing::KilledBySignal(SIGABRT), "Check failed.*");
  // We have to cast kint64max to double to avoid a warning on implicit cast that changes the value.
  EXPECT_EXIT(MonoDelta::FromSeconds(static_cast<double>(std::numeric_limits<int64_t>::max())),
              ::testing::KilledBySignal(SIGABRT), "Check failed.*");
  EXPECT_EXIT(MonoDelta::FromMicroseconds(std::numeric_limits<int64_t>::max()),
              ::testing::KilledBySignal(SIGABRT), "Check failed.*");
}

TEST(TestMonoTime, TestCondition) {
  std::mutex mutex;
  std::condition_variable cond;
  std::unique_lock<std::mutex> lock(mutex);
  auto kWaitTime = 500ms;
#ifndef __APPLE__
  auto kAllowedError = 50ms;
#else
  auto kAllowedError = 200ms;
#endif
  for (;;) {
    auto start = CoarseMonoClock::Now();
    if (cond.wait_until(lock, CoarseMonoClock::Now() + kWaitTime) != std::cv_status::timeout) {
      continue;
    }
    auto end = CoarseMonoClock::Now();
    LOG(INFO) << "Passed: " << ToSeconds(end - start) << " seconds.";
    ASSERT_GE(end - start, kWaitTime - kAllowedError);
    ASSERT_LE(end - start, kWaitTime + kAllowedError);
    break;
  }
}

TEST(TestMonoTime, TestSubtractDelta) {
  MonoTime start = MonoTime::Now();
  MonoDelta delta = MonoDelta::FromMilliseconds(100);
  ASSERT_GT(start, start - delta);
  MonoTime start_copy = start;
  start_copy.SubtractDelta(delta);
  ASSERT_EQ(start_copy, start - delta);
  ASSERT_EQ(start_copy + delta, start);
}

} // namespace yb
