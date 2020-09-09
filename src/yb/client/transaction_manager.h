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

#ifndef YB_CLIENT_TRANSACTION_MANAGER_H
#define YB_CLIENT_TRANSACTION_MANAGER_H

#include <functional>
#include <memory>

#include "yb/client/client_fwd.h"

#include "yb/common/clock.h"
#include "yb/common/hybrid_time.h"

#include "yb/rpc/rpc_fwd.h"

#include "yb/util/result.h"

namespace yb {
namespace client {

typedef std::function<void(const Result<std::string>&)> PickStatusTabletCallback;

// TransactionManager manages multiple transactions. It lives at the YQL engine layer.
class TransactionManager {
 public:
  TransactionManager(YBClient* client, const scoped_refptr<ClockBase>& clock,
                     LocalTabletFilter local_tablet_filter);
  ~TransactionManager();

  TransactionManager(TransactionManager&& rhs);
  TransactionManager& operator=(TransactionManager&& rhs);

  void PickStatusTablet(PickStatusTabletCallback callback);

  rpc::Rpcs& rpcs();
  YBClient* client() const;

  const scoped_refptr<ClockBase>& clock() const;
  HybridTime Now() const;
  HybridTimeRange NowRange() const;

  void UpdateClock(HybridTime time);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace client
} // namespace yb

#endif // YB_CLIENT_TRANSACTION_MANAGER_H
