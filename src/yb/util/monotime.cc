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

#include <limits>
#include <glog/logging.h>

#include "yb/gutil/mathlimits.h"
#include "yb/gutil/stringprintf.h"
#include "yb/gutil/sysinfo.h"
#include "yb/gutil/walltime.h"
#include "yb/util/thread_restrictions.h"

using namespace std::literals;

namespace yb {

#define MAX_MONOTONIC_SECONDS \
  (((1ULL<<63) - 1ULL) /(int64_t)MonoTime::kNanosecondsPerSecond)

namespace {

bool SafeToAdd64(int64_t a, int64_t b) {
  bool negativeA = a < 0;
  bool negativeB = b < 0;
  if (negativeA != negativeB) {
    return true;
  }
  bool negativeSum = (a + b) < 0;
  return negativeSum == negativeA;
}

} // namespace

///
/// MonoDelta
///

const MonoDelta::NanoDeltaType MonoDelta::kUninitialized =
    std::numeric_limits<NanoDeltaType>::min();
const MonoDelta MonoDelta::kMin = MonoDelta(std::numeric_limits<NanoDeltaType>::min() + 1);
const MonoDelta MonoDelta::kMax = MonoDelta(std::numeric_limits<NanoDeltaType>::max());
const MonoDelta MonoDelta::kZero = MonoDelta(0);

MonoDelta MonoDelta::FromSeconds(double seconds) {
  CHECK_LE(seconds, std::numeric_limits<int64_t>::max() / MonoTime::kNanosecondsPerSecond);
  int64_t delta = seconds * MonoTime::kNanosecondsPerSecond;
  return MonoDelta(delta);
}

MonoDelta MonoDelta::FromMilliseconds(int64_t ms) {
  CHECK_LE(ms, std::numeric_limits<int64_t>::max() / MonoTime::kNanosecondsPerMillisecond);
  return MonoDelta(ms * MonoTime::kNanosecondsPerMillisecond);
}

MonoDelta MonoDelta::FromMicroseconds(int64_t us) {
  CHECK_LE(us, std::numeric_limits<int64_t>::max() / MonoTime::kNanosecondsPerMicrosecond);
  return MonoDelta(us * MonoTime::kNanosecondsPerMicrosecond);
}

MonoDelta MonoDelta::FromNanoseconds(int64_t ns) {
  return MonoDelta(ns);
}

MonoDelta::MonoDelta() noexcept : nano_delta_(kUninitialized) {
}

bool MonoDelta::Initialized() const {
  return nano_delta_ != kUninitialized;
}

bool MonoDelta::LessThan(const MonoDelta &rhs) const {
  DCHECK(Initialized());
  DCHECK(rhs.Initialized());
  return nano_delta_ < rhs.nano_delta_;
}

bool MonoDelta::MoreThan(const MonoDelta &rhs) const {
  DCHECK(Initialized());
  DCHECK(rhs.Initialized());
  return nano_delta_ > rhs.nano_delta_;
}

bool MonoDelta::Equals(const MonoDelta &rhs) const {
  DCHECK(Initialized());
  DCHECK(rhs.Initialized());
  return nano_delta_ == rhs.nano_delta_;
}

bool MonoDelta::IsNegative() const {
  DCHECK(Initialized());
  return nano_delta_ < 0;
}

std::string MonoDelta::ToString() const {
  return Initialized() ? StringPrintf("%.3fs", ToSeconds()) : "<uninitialized>";
}

MonoDelta::MonoDelta(int64_t delta)
  : nano_delta_(delta) {
}

double MonoDelta::ToSeconds() const {
  DCHECK(Initialized());
  double d(nano_delta_);
  d /= MonoTime::kNanosecondsPerSecond;
  return d;
}

double MonoDelta::ToMinutes() const {
  auto seconds = ToSeconds();
  return seconds / MonoTime::kSecondsPerMinute;
}

int64_t MonoDelta::ToNanoseconds() const {
  DCHECK(Initialized());
  return nano_delta_;
}

std::chrono::steady_clock::duration MonoDelta::ToSteadyDuration() const {
  return std::chrono::nanoseconds(ToNanoseconds());
}

int64_t MonoDelta::ToMicroseconds() const {
  DCHECK(Initialized());
  return nano_delta_ / MonoTime::kNanosecondsPerMicrosecond;
}

int64_t MonoDelta::ToMilliseconds() const {
  DCHECK(Initialized());
  return nano_delta_ / MonoTime::kNanosecondsPerMillisecond;
}

MonoDelta& MonoDelta::operator+=(const MonoDelta& rhs) {
  DCHECK(Initialized());
  DCHECK(rhs.Initialized());
  DCHECK(SafeToAdd64(nano_delta_, rhs.nano_delta_));
  DCHECK(nano_delta_ + rhs.nano_delta_ != kUninitialized);
  nano_delta_ += rhs.nano_delta_;
  return *this;
}

MonoDelta& MonoDelta::operator-=(const MonoDelta& rhs) {
  DCHECK(Initialized());
  DCHECK(rhs.Initialized());
  DCHECK(SafeToAdd64(nano_delta_, -rhs.nano_delta_));
  DCHECK(nano_delta_ - rhs.nano_delta_ != kUninitialized);
  nano_delta_ -= rhs.nano_delta_;
  return *this;
}

MonoDelta& MonoDelta::operator*=(int64_t mul) {
  DCHECK(Initialized());
  DCHECK(mul == 0 || (nano_delta_ * mul / mul == nano_delta_)) // Check for overflow
      << "Mul: " << mul << ", nano_delta_: " << nano_delta_;
  DCHECK(nano_delta_ * mul != kUninitialized);
  nano_delta_ *= mul;
  return *this;
}

MonoDelta& MonoDelta::operator/=(int64_t divisor) {
  DCHECK(Initialized());
  DCHECK_NE(divisor, 0);
  nano_delta_ /= divisor;
  return *this;
}

void MonoDelta::ToTimeVal(struct timeval *tv) const {
  DCHECK(Initialized());
  tv->tv_sec = nano_delta_ / MonoTime::kNanosecondsPerSecond;
  tv->tv_usec = (nano_delta_ - (tv->tv_sec * MonoTime::kNanosecondsPerSecond))
      / MonoTime::kNanosecondsPerMicrosecond;

  // tv_usec must be between 0 and 999999.
  // There is little use for negative timevals so wrap it in PREDICT_FALSE.
  if (PREDICT_FALSE(tv->tv_usec < 0)) {
    --(tv->tv_sec);
    tv->tv_usec += 1000000;
  }

  // Catch positive corner case where we "round down" and could potentially set a timeout of 0.
  // Make it 1 usec.
  if (PREDICT_FALSE(tv->tv_usec == 0 && tv->tv_sec == 0 && nano_delta_ > 0)) {
    tv->tv_usec = 1;
  }

  // Catch negative corner case where we "round down" and could potentially set a timeout of 0.
  // Make it -1 usec (but normalized, so tv_usec is not negative).
  if (PREDICT_FALSE(tv->tv_usec == 0 && tv->tv_sec == 0 && nano_delta_ < 0)) {
    tv->tv_sec = -1;
    tv->tv_usec = 999999;
  }
}


void MonoDelta::NanosToTimeSpec(int64_t nanos, struct timespec* ts) {
  ts->tv_sec = nanos / MonoTime::kNanosecondsPerSecond;
  ts->tv_nsec = nanos - (ts->tv_sec * MonoTime::kNanosecondsPerSecond);

  // tv_nsec must be between 0 and 999999999.
  // There is little use for negative timespecs so wrap it in PREDICT_FALSE.
  if (PREDICT_FALSE(ts->tv_nsec < 0)) {
    --(ts->tv_sec);
    ts->tv_nsec += MonoTime::kNanosecondsPerSecond;
  }
}

void MonoDelta::ToTimeSpec(struct timespec *ts) const {
  DCHECK(Initialized());
  NanosToTimeSpec(nano_delta_, ts);
}

///
/// MonoTime
///

const MonoTime MonoTime::kMin = MonoTime::Min();
const MonoTime MonoTime::kMax = MonoTime::Max();
const MonoTime MonoTime::kUninitialized = MonoTime();

MonoTime MonoTime::Now() {
  return MonoTime(std::chrono::steady_clock::now());
}

MonoTime MonoTime::Max() {
  return MonoTime(std::chrono::steady_clock::time_point::max());
}

MonoTime MonoTime::Min() {
  return MonoTime(std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(1)));
}

bool MonoTime::IsMax() const {
  return Equals(kMax);
}

bool MonoTime::IsMin() const {
  return Equals(kMin);
}

const MonoTime& MonoTime::Earliest(const MonoTime& a, const MonoTime& b) {
  return std::min(a, b);
}

MonoDelta MonoTime::GetDeltaSince(const MonoTime &rhs) const {
  DCHECK(Initialized());
  DCHECK(rhs.Initialized());
  return MonoDelta(value_ - rhs.value_);
}

void MonoTime::AddDelta(const MonoDelta &delta) {
  DCHECK(Initialized());
  DCHECK(delta.Initialized());
  if (delta == MonoDelta::kMax) {
    value_ = kMax.value_;
  } else {
    value_ += delta.ToSteadyDuration();
  }
}

void MonoTime::SubtractDelta(const MonoDelta &delta) {
  DCHECK(Initialized());
  DCHECK(delta.Initialized());
  if (delta == MonoDelta::kMin) {
    value_ = kMin.value_;
  } else {
    value_ -= delta.ToSteadyDuration();
  }
}

bool MonoTime::ComesBefore(const MonoTime &rhs) const {
  DCHECK(Initialized());
  DCHECK(rhs.Initialized());
  return value_ < rhs.value_;
}

std::string MonoTime::ToString() const {
  if (!Initialized())
    return "MonoTime::kUninitialized";
  if (IsMax())
    return "MonoTime::kMax";
  if (IsMin())
    return "MonoTime::kMin";
  return StringPrintf("%.3fs", ToSeconds());
}

bool MonoTime::Equals(const MonoTime& other) const {
  return value_ == other.value_;
}

double MonoTime::ToSeconds() const {
  return yb::ToSeconds(value_.time_since_epoch());
}

void MonoTime::MakeAtLeast(MonoTime rhs) {
  if (rhs.Initialized() && (!Initialized() || value_ < rhs.value_)) {
    value_ = rhs.value_;
  }
}

// ------------------------------------------------------------------------------------------------

std::string FormatForComparisonFailureMessage(const MonoDelta& op, const MonoDelta& other) {
  return op.ToString();
}

void SleepFor(const MonoDelta& delta) {
  ThreadRestrictions::AssertWaitAllowed();
  base::SleepForNanoseconds(delta.ToNanoseconds());
}

CoarseMonoClock::time_point CoarseMonoClock::now() {
#if defined(__APPLE__)
  int64_t nanos = walltime_internal::GetMonoTimeNanos();
# else
  struct timespec ts;
  PCHECK(clock_gettime(CLOCK_MONOTONIC_COARSE, &ts) == 0);
  CHECK_LT(ts.tv_sec, MAX_MONOTONIC_SECONDS);
  int64_t nanos = static_cast<int64_t>(ts.tv_sec) * MonoTime::kNanosecondsPerSecond + ts.tv_nsec;
#endif // defined(__APPLE__)
  return time_point(duration(nanos));
}

std::string ToString(CoarseMonoClock::TimePoint time_point) {
  return MonoDelta(time_point.time_since_epoch()).ToString();
}

CoarseTimePoint ToCoarse(MonoTime monotime) {
  return CoarseTimePoint(monotime.ToSteadyTimePoint().time_since_epoch());
}

std::chrono::steady_clock::time_point ToSteady(CoarseTimePoint time_point) {
  return std::chrono::steady_clock::time_point(time_point.time_since_epoch());
}

} // namespace yb
