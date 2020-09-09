//
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
//

#ifndef YB_RPC_IO_THREAD_POOL_H
#define YB_RPC_IO_THREAD_POOL_H

#include <memory>
#include <functional>

#include "yb/rpc/rpc_fwd.h"
#include "yb/util/net/net_fwd.h"

namespace yb {
namespace rpc {

// Runs io service in specified number of threads.
class IoThreadPool {
 public:
  IoThreadPool(const std::string& name, size_t num_threads);
  ~IoThreadPool();

  void Shutdown();
  void Join();

  IoService& io_service();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace rpc
} // namespace yb

#endif // YB_RPC_IO_THREAD_POOL_H
