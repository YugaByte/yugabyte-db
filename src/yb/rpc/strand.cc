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

#include "yb/rpc/strand.h"

#include <thread>

using namespace std::literals;

namespace yb {
namespace rpc {

namespace {

const Status& StrandAbortedStatus() {
  static const Status result = STATUS(Aborted, "Strand shutdown");
  return result;
}

}

Strand::Strand(ThreadPool* thread_pool) : thread_pool_(*thread_pool) {}

void Strand::Enqueue(StrandTask* task) {
  if (closing_.load(std::memory_order_acquire)) {
    task->Done(STATUS(Aborted, "Strand closing"));
    return;
  }

  active_tasks_.fetch_add(1, std::memory_order_release);
  queue_.Push(task);

  bool expected = false;
  if (running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    thread_pool_.Enqueue(this);
  }
}

void Strand::Shutdown() {
  bool expected = false;
  if (!closing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    LOG(DFATAL) << "Strand already closed";
    return;
  }

  while (active_tasks_.load(std::memory_order_acquire) ||
         running_.load(std::memory_order_acquire)) {
    // We expected shutdown to happen rarely, so just use busy wait here.
    std::this_thread::sleep_for(1ms);
  }
}

void Strand::Run() {
  // Actual work is performed in Done.
  // Because we need `Status`, i.e. if `Strand` task aborted, because of thread pool shutdown.
  // We should abort all enqueued tasks.
}

void Strand::Done(const Status& status) {
  for (;;) {
    while (StrandTask *task = queue_.Pop()) {
      active_tasks_.fetch_sub(1, std::memory_order_release);

      const auto& actual_status =
          !closing_.load(std::memory_order_acquire) ? status : StrandAbortedStatus();
      if (actual_status.ok()) {
        task->Run();
      }
      task->Done(actual_status);
    }
    running_.store(false, std::memory_order_release);
    // Check whether tasks have been added while we were setting running to false.
    if (active_tasks_.load(std::memory_order_acquire)) {
      // Got more operations, try stay in the loop.
      bool expected = false;
      if (running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        continue;
      }
      // If someone else has flipped running_ to true, we can safely exit this function because
      // another task is already submitted to thread pool.
    }
    break;
  }
}

} // namespace rpc
} // namespace yb
