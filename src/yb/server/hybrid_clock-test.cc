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

#include <algorithm>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include "yb/server/hybrid_clock.h"
#include "yb/server/mock_hybrid_clock.h"
#include "yb/util/monotime.h"
#include "yb/util/random.h"
#include "yb/util/random_util.h"
#include "yb/util/test_util.h"

DECLARE_uint64(max_clock_sync_error_usec);
DECLARE_bool(disable_clock_sync_error);

namespace yb {
namespace server {

class HybridClockTest : public YBTest {
 public:
  HybridClockTest()
      : clock_(new HybridClock()) {
  }

  void SetUp() override {
    YBTest::SetUp();
    ASSERT_OK(clock_->Init());
  }

 protected:
  void RunMultiThreadedTest(int num_reads_per_update);

  scoped_refptr<HybridClock> clock_;
};

TEST(MockHybridClockTest, TestMockedSystemClock) {
  ASSERT_EQ(kMaxHybridTimePhysicalMicros, HybridTime::kMax.GetPhysicalValueMicros());
  MockClock mock_clock;
  scoped_refptr<HybridClock> clock(new HybridClock(mock_clock.AsClock()));
  ASSERT_OK(clock->Init());
  HybridTime hybrid_time;
  uint64_t max_error_usec;
  clock->NowWithError(&hybrid_time, &max_error_usec);
  ASSERT_EQ(hybrid_time.ToUint64(), 0);
  ASSERT_EQ(max_error_usec, 0);
  // If we read the clock again we should see the logical component be incremented.
  clock->NowWithError(&hybrid_time, &max_error_usec);
  ASSERT_EQ(hybrid_time.ToUint64(), 1);
  // Now set an arbitrary time and check that is the time returned by the clock.
  PhysicalTime time = {1234, 100 * 1000};
  mock_clock.Set(time);
  clock->NowWithError(&hybrid_time, &max_error_usec);
  ASSERT_EQ(hybrid_time.ToUint64(),
            HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(time.time_point, 0).ToUint64());
  ASSERT_EQ(max_error_usec, time.max_error);
  // Perform another read, we should observe the logical component increment, again.
  clock->NowWithError(&hybrid_time, &max_error_usec);
  ASSERT_EQ(hybrid_time.ToUint64(),
            HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(time.time_point, 1).ToUint64());
}

// Test that two subsequent time reads are monotonically increasing.
TEST_F(HybridClockTest, TestNow_ValuesIncreaseMonotonically) {
  const HybridTime now1 = clock_->Now();
  const HybridTime now2 = clock_->Now();
  ASSERT_LT(now1.value(), now2.value());
}

// Tests the clock updates with the incoming value if it is higher.
TEST_F(HybridClockTest, TestUpdate_LogicalValueIncreasesByAmount) {
  HybridTime now = clock_->Now();
  uint64_t now_micros = HybridClock::GetPhysicalValueMicros(now);

  // increase the logical value
  uint64_t logical = HybridClock::GetLogicalValue(now);
  logical += 10;

  // increase the physical value so that we're sure the clock will take this
  // one, 200 msecs should be more than enough.
  now_micros += 200000;

  HybridTime now_increased = HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(
      now_micros, logical);

  clock_->Update(now_increased);

  HybridTime now2 = clock_->Now();
  ASSERT_EQ(logical + 1, HybridClock::GetLogicalValue(now2));
  ASSERT_EQ(HybridClock::GetPhysicalValueMicros(now) + 200000,
            HybridClock::GetPhysicalValueMicros(now2));
}

// Thread which loops polling the clock and updating it slightly
// into the future.
void StresserThread(HybridClock* clock, AtomicBool* stop, int num_reads_per_update) {
  Random rng(GetRandomSeed32());
  HybridTime prev = HybridTime::kMin;
  while (!stop->Load()) {
    HybridTime t;
    for (int i = 0; i < num_reads_per_update; ++i) {
      t = clock->Now();
      ASSERT_GT(t, prev);
      prev = t;
    }

    // Add a random bit of offset to the clock, and perform an update.
    HybridTime new_ht = HybridClock::AddPhysicalTimeToHybridTime(
        t, MonoDelta::FromMicroseconds(rng.Uniform(10000)));
    clock->Update(new_ht);
    prev = new_ht;
  }
}

// Regression test for KUDU-953: if threads are updating and polling the
// clock concurrently, the clock should still never run backwards.
TEST_F(HybridClockTest, TestClockDoesntGoBackwardsWithUpdates) {
  RunMultiThreadedTest(1);
}

TEST_F(HybridClockTest, TestClockDoesntGoBackwardsWithOccasionalUpdates) {
  RunMultiThreadedTest(1000000);
}

void HybridClockTest::RunMultiThreadedTest(int num_reads_per_update) {
  vector<scoped_refptr<yb::Thread> > threads;

  AtomicBool stop(false);
  for (int i = 0; i < 4; i++) {
    scoped_refptr<Thread> thread;
    ASSERT_OK(Thread::Create("test", "stresser",
                             &StresserThread, clock_.get(), &stop, num_reads_per_update,
                             &thread));
    threads.push_back(thread);
  }

  SleepFor(MonoDelta::FromSeconds(10));
  stop.Store(true);
  for (const scoped_refptr<Thread>& t : threads) {
    t->Join();
  }
}

TEST_F(HybridClockTest, CompareHybridClocksToDelta) {
  EXPECT_EQ(1, HybridClock::CompareHybridClocksToDelta(
      HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1000, 10),
      HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1002, 10),
      MonoDelta::FromMicroseconds(1)));

  EXPECT_EQ(-1, HybridClock::CompareHybridClocksToDelta(
      HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1000, 10),
      HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1002, 10),
      MonoDelta::FromMicroseconds(5)));

  EXPECT_EQ(0, HybridClock::CompareHybridClocksToDelta(
      HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1000, 10),
      HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1001, 10),
      MonoDelta::FromMicroseconds(1)));

  EXPECT_EQ(1, HybridClock::CompareHybridClocksToDelta(
      HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1000, 10),
      HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1001, 11),
      MonoDelta::FromMicroseconds(1)));

  EXPECT_EQ(-1, HybridClock::CompareHybridClocksToDelta(
      HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1000, 10),
      HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1001, 9),
      MonoDelta::FromMicroseconds(1)));

  EXPECT_EQ(-1, HybridClock::CompareHybridClocksToDelta(
      HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1000, 10),
      HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1000, 10),
      MonoDelta::FromNanoseconds(MonoTime::kNanosecondsPerMicrosecond - 1)));

  EXPECT_EQ(-1, HybridClock::CompareHybridClocksToDelta(
      HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1000, 10),
      HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1001, 10),
      MonoDelta::FromNanoseconds(MonoTime::kNanosecondsPerMicrosecond + 1)));

  ASSERT_NO_FATALS(HybridClock::GetPhysicalValueNanos(
      HybridTime(std::numeric_limits<uint64_t>::max())));

  EXPECT_EQ(-1, HybridClock::CompareHybridClocksToDelta(
      HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1000, 10),
      HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1000, 9),
      MonoDelta::FromMicroseconds(1)));
}

}  // namespace server
}  // namespace yb
