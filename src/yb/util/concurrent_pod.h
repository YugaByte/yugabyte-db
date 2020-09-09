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

#ifndef YB_UTIL_CONCURRENT_POD_H
#define YB_UTIL_CONCURRENT_POD_H

namespace yb {

// Concurrent wrapper for POD objects that does not fit into std::atomic.
// Designed for frequent reads with rare writes.
// Usual cost of read is 2 atomic load + copy of value.
template <class T>
class ConcurrentPod {
 public:
  explicit ConcurrentPod(const std::chrono::steady_clock::duration& timeout)
      : timeout_(timeout) {}

  void Store(const T& value) {
    time_.store(CoarseTimePoint::min(), std::memory_order_release);
    value_ = value;
    time_.store(CoarseMonoClock::now(), std::memory_order_release);
  }

  T Load() const {
    for (;;) {
      auto time1 = time_.load(std::memory_order_acquire);
      ANNOTATE_IGNORE_READS_BEGIN();
      auto result = value_;
      ANNOTATE_IGNORE_READS_END();
      auto time2 = time_.load(std::memory_order_acquire);
      if (time1 == time2 && time1 != CoarseTimePoint::min()) {
        if (CoarseMonoClock::now() > time1 + timeout_) {
          return T();
        }
        return result;
      }
    }
  }
 private:
  const std::chrono::steady_clock::duration timeout_;
  T value_;
  std::atomic<CoarseTimePoint> time_{CoarseTimePoint::min() + MonoDelta::FromMilliseconds(1)};
};

} // namespace yb

#endif // YB_UTIL_CONCURRENT_POD_H
