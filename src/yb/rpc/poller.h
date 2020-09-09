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

#ifndef YB_RPC_POLLER_H
#define YB_RPC_POLLER_H

#include <condition_variable>
#include <mutex>

#include "yb/rpc/rpc_fwd.h"

#include "yb/gutil/thread_annotations.h"

#include "yb/util/monotime.h"
#include "yb/util/status.h"

namespace yb {
namespace rpc {

// Utility class to invoke specified callback with defined interval using scheduler.
// TODO Add separate test for poller.
class Poller {
 public:
  explicit Poller(const std::string& log_prefix, std::function<void()> callback);

  explicit Poller(std::function<void()> callback)
      : Poller(/* log_prefix= */ std::string(), std::move(callback)) {}

  Poller(Poller&&) = delete;
  void operator=(Poller&&) = delete;

  void Start(Scheduler* scheduler, MonoDelta interval);
  void Shutdown();

 private:
  void Schedule() REQUIRES(mutex_);
  void Poll(const Status& status);

  const std::string& LogPrefix() {
    return log_prefix_;
  }

  const std::string log_prefix_;
  const std::function<void()> callback_;

  Scheduler* scheduler_ = nullptr;
  MonoDelta interval_;

  std::mutex mutex_;
  bool closing_ GUARDED_BY(mutex_) = false;
  rpc::ScheduledTaskId poll_task_id_ GUARDED_BY(mutex_);
  std::condition_variable cond_ GUARDED_BY(mutex_);
};

} // namespace rpc
} // namespace yb

#endif // YB_RPC_POLLER_H
