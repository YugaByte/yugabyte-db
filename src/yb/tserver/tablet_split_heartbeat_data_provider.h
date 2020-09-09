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

#ifndef YB_TSERVER_TABLET_SPLIT_HEARTBEAT_DATA_PROVIDER_H
#define YB_TSERVER_TABLET_SPLIT_HEARTBEAT_DATA_PROVIDER_H

#include <memory>

#include "yb/tserver/heartbeater.h"

namespace yb {
namespace tserver {

class TabletSplitHeartbeatDataProvider : public PeriodicalHeartbeatDataProvider {
 public:
  explicit TabletSplitHeartbeatDataProvider(TabletServer* server);

 private:
  void DoAddData(
      const master::TSHeartbeatResponsePB& last_resp, master::TSHeartbeatRequestPB* req) override;
};

} // namespace tserver
} // namespace yb

#endif // YB_TSERVER_TABLET_SPLIT_HEARTBEAT_DATA_PROVIDER_H
