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

#ifndef YB_UTIL_CROSS_THREAD_MUTEX_H_
#define YB_UTIL_CROSS_THREAD_MUTEX_H_

#include <mutex>
#include <condition_variable>

namespace yb {

// This is a wrapper around std::mutex which can be locked and unlocked from different threads.
class CrossThreadMutex {
 public:
  void lock();

  void unlock();

  template <class Rep, class Period>
  bool try_lock_for(const std::chrono::duration<Rep, Period>& duration);

  template <class Clock, class Duration>
  bool try_lock_until(const std::chrono::time_point<Clock, Duration>& deadline);

 private:
  auto NotLockedLambda() {
    return [this] { return !is_locked_; };
  }

  std::mutex mutex_;
  std::condition_variable condition_variable_;
  bool is_locked_ = false;
};

template <class Rep, class Period>
bool CrossThreadMutex::try_lock_for(const std::chrono::duration<Rep, Period>& duration) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!condition_variable_.wait_for(lock, duration, NotLockedLambda())) {
    return false;
  }
  is_locked_ = true;
  return true;
}

template <class Clock, class Duration>
bool CrossThreadMutex::try_lock_until(const std::chrono::time_point<Clock, Duration>& deadline) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!condition_variable_.wait_until(lock, deadline, NotLockedLambda())) {
    return false;
  }
  is_locked_ = true;
  return true;
}

}  // namespace yb

#endif  // YB_UTIL_CROSS_THREAD_MUTEX_H_
