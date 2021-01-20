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
#ifndef YB_UTIL_MONOTIME_H
#define YB_UTIL_MONOTIME_H

#include <chrono>
#include <cstdint>
#include <string>

#ifdef YB_HEADERS_NO_STUBS
#include <gtest/gtest_prod.h>
#else
// This is a poor module interdependency, but the stubs are header-only and
// it's only for exported header builds, so we'll make an exception.
#include "yb/client/stubs.h"
#endif



struct timeval;
struct timespec;

namespace yb {
class MonoTime;

// Represent an elapsed duration of time -- i.e the delta between
// two MonoTime instances.
//
// A MonoDelta built with the default constructor is "uninitialized" and
// may not be used for any operation.
class MonoDelta {
 public:
  static MonoDelta FromSeconds(double seconds);
  static MonoDelta FromMilliseconds(int64_t ms);
  static MonoDelta FromMicroseconds(int64_t us);
  static MonoDelta FromNanoseconds(int64_t ns);

  static const MonoDelta kMin;
  static const MonoDelta kMax;
  static const MonoDelta kZero;

  MonoDelta() noexcept;

  template<class Rep, class Period>
  MonoDelta(const std::chrono::duration<Rep, Period>& duration) // NOLINT
      : nano_delta_(std::chrono::nanoseconds(duration).count()) {}

  bool Initialized() const;
  bool LessThan(const MonoDelta &rhs) const;
  bool MoreThan(const MonoDelta &rhs) const;
  bool Equals(const MonoDelta &rhs) const;
  bool IsNegative() const;
  std::string ToString() const;
  double ToSeconds() const;
  double ToMinutes() const;
  int64_t ToMilliseconds() const;
  int64_t ToMicroseconds() const;
  int64_t ToNanoseconds() const;
  std::chrono::steady_clock::duration ToSteadyDuration() const;

  MonoDelta& operator+=(const MonoDelta& rhs);
  MonoDelta& operator-=(const MonoDelta& rhs);
  MonoDelta& operator*=(int64_t mul);
  MonoDelta& operator/=(int64_t mul);

  // Update struct timeval to current value of delta, with microsecond accuracy.
  // Note that if MonoDelta::IsPositive() returns true, the struct timeval
  // is guaranteed to hold a positive number as well (at least 1 microsecond).
  void ToTimeVal(struct timeval *tv) const;

  // Update struct timespec to current value of delta, with nanosecond accuracy.
  void ToTimeSpec(struct timespec *ts) const;

  // Convert a nanosecond value to a timespec.
  static void NanosToTimeSpec(int64_t nanos, struct timespec* ts);

  explicit operator bool() const { return Initialized(); }
  bool operator !() const { return !Initialized(); }

  MonoDelta operator-() const { return MonoDelta(-nano_delta_); }

 private:
  typedef int64_t NanoDeltaType;
  static const NanoDeltaType kUninitialized;

  FRIEND_TEST(TestMonoTime, TestDeltaConversions);
  explicit MonoDelta(NanoDeltaType delta);
  NanoDeltaType nano_delta_;
};

inline bool operator<(MonoDelta lhs, MonoDelta rhs) { return lhs.LessThan(rhs); }
inline bool operator>(MonoDelta lhs, MonoDelta rhs) { return rhs < lhs; }
inline bool operator>=(MonoDelta lhs, MonoDelta rhs) { return !(lhs < rhs); }
inline bool operator<=(MonoDelta lhs, MonoDelta rhs) { return !(rhs < lhs); }

inline bool operator==(MonoDelta lhs, MonoDelta rhs) { return lhs.Equals(rhs); }
inline bool operator!=(MonoDelta lhs, MonoDelta rhs) { return !(rhs == lhs); }

std::string FormatForComparisonFailureMessage(const MonoDelta& op, const MonoDelta& other);

inline MonoDelta operator-(MonoDelta lhs, MonoDelta rhs) { return lhs -= rhs; }
inline MonoDelta operator+(MonoDelta lhs, MonoDelta rhs) { return lhs += rhs; }
inline MonoDelta operator*(MonoDelta lhs, int64_t rhs) { return lhs *= rhs; }
inline MonoDelta operator/(MonoDelta lhs, int64_t rhs) { return lhs /= rhs; }

inline std::ostream& operator<<(std::ostream& out, MonoDelta delta) {
  return out << delta.ToString();
}

// Represent a particular point in time, relative to some fixed but unspecified
// reference point.
//
// This time is monotonic, meaning that if the user changes his or her system
// clock, the monotime does not change.
class MonoTime {
 public:
  static constexpr int64_t kNanosecondsPerMicrosecond = 1000L;
  static constexpr int64_t kMicrosecondsPerMillisecond = 1000L;
  static constexpr int64_t kMillisecondsPerSecond = 1000L;
  static constexpr int64_t kSecondsPerMinute = 60L;

  static constexpr int64_t kNanosecondsPerMillisecond =
      kNanosecondsPerMicrosecond * kMicrosecondsPerMillisecond;

  static constexpr int64_t kMicrosecondsPerSecond =
      kMillisecondsPerSecond * kMicrosecondsPerMillisecond;

  static constexpr int64_t kNanosecondsPerSecond =
      kNanosecondsPerMillisecond * kMillisecondsPerSecond;

  static const MonoTime kMin;
  static const MonoTime kMax;
  static const MonoTime kUninitialized;

  // The coarse monotonic time is faster to retrieve, but "only" accurate to within a millisecond or
  // two.  The speed difference will depend on your timer hardware.
  static MonoTime Now();

  // Return MonoTime equal to farthest possible time into the future.
  static MonoTime Max();

  // Return MonoTime equal to farthest possible time into the past.
  static MonoTime Min();

  // Return the earliest (minimum) of the two monotimes.
  static const MonoTime& Earliest(const MonoTime& a, const MonoTime& b);

  MonoTime() noexcept {}
  MonoTime(std::chrono::steady_clock::time_point value) : value_(value) {} // NOLINT

  bool Initialized() const { return value_ != std::chrono::steady_clock::time_point(); }

  MonoDelta GetDeltaSince(const MonoTime &rhs) const;
  MonoDelta GetDeltaSinceMin() const { return GetDeltaSince(Min()); }
  void AddDelta(const MonoDelta &delta);
  void SubtractDelta(const MonoDelta &delta);
  bool ComesBefore(const MonoTime &rhs) const;
  std::string ToString() const;
  bool Equals(const MonoTime& other) const;
  bool IsMax() const;
  bool IsMin() const;

  uint64_t ToUint64() const { return value_.time_since_epoch().count(); }
  static MonoTime FromUint64(uint64_t value) {
    return MonoTime(std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(
        value)));
  }

  explicit operator bool() const { return Initialized(); }
  bool operator !() const { return !Initialized(); }

  // Set this time to the given value if it is lower than that or uninitialized.
  void MakeAtLeast(MonoTime rhs);

  std::chrono::steady_clock::time_point ToSteadyTimePoint() const {
    return value_;
  }

 private:
  double ToSeconds() const;

  std::chrono::steady_clock::time_point value_;
};

inline MonoTime& operator+=(MonoTime& lhs, const MonoDelta& rhs) { // NOLINT
  lhs.AddDelta(rhs);
  return lhs;
}

inline MonoTime operator+(MonoTime lhs, const MonoDelta& rhs) {
  lhs += rhs;
  return lhs;
}

template <class Clock>
inline auto operator+(const std::chrono::time_point<Clock>& lhs, const MonoDelta& rhs) {
  return lhs + rhs.ToSteadyDuration();
}

inline MonoDelta operator-(const MonoTime& lhs, const MonoTime& rhs) {
  return lhs.GetDeltaSince(rhs);
}

inline MonoTime& operator-=(MonoTime& lhs, const MonoDelta& rhs) { // NOLINT
  lhs.SubtractDelta(rhs);
  return lhs;
}

inline MonoTime operator-(const MonoTime& lhs, const MonoDelta& rhs) {
  MonoTime result = lhs;
  result.AddDelta(-rhs);
  return MonoTime(result);
}

inline bool operator<(const MonoTime& lhs, const MonoTime& rhs) {
  return lhs.ComesBefore(rhs);
}

inline bool operator>(const MonoTime& lhs, const MonoTime& rhs) { return rhs < lhs; }
inline bool operator<=(const MonoTime& lhs, const MonoTime& rhs) { return !(rhs < lhs); }
inline bool operator>=(const MonoTime& lhs, const MonoTime& rhs) { return !(lhs < rhs); }

inline bool operator==(const MonoTime& lhs, const MonoTime& rhs) { return lhs.Equals(rhs); }
inline bool operator!=(const MonoTime& lhs, const MonoTime& rhs) { return !(lhs == rhs); }

// Sleep for a MonoDelta duration.
//
// This is preferred over sleep(3), usleep(3), and nanosleep(3). It's less prone to mixups with
// units since it uses a MonoDelta. It also ignores EINTR, so will reliably sleep at least the
// MonoDelta duration.
void SleepFor(const MonoDelta& delta);

class CoarseMonoClock {
 public:
  typedef std::chrono::nanoseconds duration;
  typedef duration Duration;
  typedef std::chrono::time_point<CoarseMonoClock> time_point;
  typedef time_point TimePoint;

  static constexpr bool is_steady = true;

  static time_point now();
  static TimePoint Now() { return now(); }
};

typedef CoarseMonoClock::TimePoint CoarseTimePoint;
typedef CoarseMonoClock::Duration CoarseDuration;

template <class Rep, class Period>
int64_t ToMilliseconds(const std::chrono::duration<Rep, Period>& duration) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

template <class Rep, class Period>
int64_t ToMicroseconds(const std::chrono::duration<Rep, Period>& duration) {
  return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

template <class Rep, class Period>
int64_t ToNanoseconds(const std::chrono::duration<Rep, Period>& duration) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

template <class Rep, class Period>
double ToSeconds(const std::chrono::duration<Rep, Period>& duration) {
  return duration.count() /
      static_cast<double>(std::chrono::duration_cast<std::chrono::duration<Rep, Period>>(
          std::chrono::seconds(1)).count());
}

inline double ToSeconds(MonoDelta delta) {
  return delta.ToSeconds();
}

std::string ToString(CoarseMonoClock::TimePoint value);

CoarseTimePoint ToCoarse(MonoTime monotime);
std::chrono::steady_clock::time_point ToSteady(CoarseTimePoint time_point);

} // namespace yb

#endif // YB_UTIL_MONOTIME_H
