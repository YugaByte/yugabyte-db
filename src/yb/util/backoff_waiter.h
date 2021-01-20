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
//

#ifndef YB_UTIL_BACKOFF_WAITER_H
#define YB_UTIL_BACKOFF_WAITER_H

#include <chrono>
#include <thread>

#include "yb/util/monotime.h"
#include "yb/util/random_util.h"

namespace yb {

// Utility class for waiting.
// It tracks number of attempts and exponentially increase sleep timeout.
template <class Clock>
class GenericBackoffWaiter {
 public:
  typedef typename Clock::time_point TimePoint;
  typedef typename Clock::duration Duration;

  // deadline - time when waiter decides that it is expired.
  // max_wait - max duration for single wait.
  // base_delay - multiplier for wait duration.
  explicit GenericBackoffWaiter(
      TimePoint deadline, Duration max_wait = Duration::max(),
      Duration base_delay = std::chrono::milliseconds(1))
      : deadline_(deadline), max_wait_(max_wait), base_delay_(base_delay) {}

  bool ExpiredNow() const {
    return ExpiredAt(Clock::now());
  }

  bool ExpiredAt(TimePoint time) const {
    return time >= deadline_;
  }

  bool Wait() {
    auto now = Clock::now();
    if (ExpiredAt(now)) {
      return false;
    }

    NextAttempt();

    std::this_thread::sleep_for(DelayForTime(now));
    return true;
  }

  void NextAttempt() {
    ++attempt_;
  }

  Duration DelayForNow() const {
    return DelayForTime(Clock::now());
  }

  Duration DelayForTime(TimePoint now) const {
    Duration max_wait = std::min(deadline_ - now, max_wait_);
    // 1st retry delayed 2^4 of base delays, 2nd 2^5 base delays, etc..
    Duration attempt_delay =
        base_delay_ *
        (attempt_ >= 29 ? std::numeric_limits<int32_t>::max() : 1LL << (attempt_ + 3));
    Duration jitter = std::chrono::milliseconds(RandomUniformInt(0, 50));
    return std::min(attempt_delay + jitter, max_wait);
  }

  size_t attempt() const {
    return attempt_;
  }

  // Resets attempt counter, w/o modifying deadline.
  void Restart() {
    attempt_ = 0;
  }

 private:
  TimePoint deadline_;
  size_t attempt_ = 0;
  Duration max_wait_;
  Duration base_delay_;
};

typedef GenericBackoffWaiter<std::chrono::steady_clock> BackoffWaiter;
typedef GenericBackoffWaiter<CoarseMonoClock> CoarseBackoffWaiter;

} // namespace yb

#endif // YB_UTIL_BACKOFF_WAITER_H
