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

#include <stdlib.h>
#include <string>

#include "yb/cdc/cdc_util.h"
#include "yb/cdc/cdc_output_client_interface.h"
#include "yb/rpc/rpc.h"
#include "yb/tserver/cdc_consumer.h"
#include "yb/tserver/tablet_server.h"
#include "yb/util/locks.h"
#include "yb/util/status.h"

#ifndef ENT_SRC_YB_TSERVER_CDC_POLLER_H
#define ENT_SRC_YB_TSERVER_CDC_POLLER_H

namespace yb {

class ThreadPool;

namespace rpc {

class RpcController;

} // namespace rpc

namespace cdc {

class CDCServiceProxy;

} // namespace cdc

namespace tserver {
namespace enterprise {

class CDCConsumer;


class CDCPoller : public std::enable_shared_from_this<CDCPoller> {
 public:
  CDCPoller(const cdc::ProducerTabletInfo& producer_tablet_info,
            const cdc::ConsumerTabletInfo& consumer_tablet_info,
            std::function<bool(void)> should_continue_polling,
            std::function<void(void)> remove_self_from_pollers_map,
            ThreadPool* thread_pool,
            const std::shared_ptr<CDCClient>& local_client,
            const std::shared_ptr<CDCClient>& producer_client,
            CDCConsumer* cdc_consumer,
            bool use_local_tserver);
  ~CDCPoller() = default;

  // Begins poll process for a producer tablet.
  void Poll();

  std::string LogPrefixUnlocked() const;

 private:
  bool CheckOnline();

  void DoPoll();
  // Does the work of sending the changes to the output client.
  void HandlePoll(yb::Status status,
                  std::shared_ptr<cdc::GetChangesResponsePB> resp);
  // Async handler for the response from output client.
  void HandleApplyChanges(cdc::OutputClientResponse response);
  // Does the work of polling for new changes.
  void DoHandleApplyChanges(cdc::OutputClientResponse response);

  cdc::ProducerTabletInfo producer_tablet_info_;
  cdc::ConsumerTabletInfo consumer_tablet_info_;
  std::function<bool()> should_continue_polling_ GUARDED_BY(data_mutex_);
  std::function<void(void)> remove_self_from_pollers_map_ GUARDED_BY(data_mutex_);

  // Although this is processing serially, it might be on a different thread in the ThreadPool.
  // Using mutex to guarantee cache flush, preventing TSAN warnings.
  std::mutex data_mutex_;

  OpIdPB op_id_ GUARDED_BY(data_mutex_);

  yb::Status status_ GUARDED_BY(data_mutex_);
  std::shared_ptr<cdc::GetChangesResponsePB> resp_ GUARDED_BY(data_mutex_);

  std::unique_ptr<cdc::CDCOutputClient> output_client_;
  std::shared_ptr<CDCClient> producer_client_;

  ThreadPool* thread_pool_;
  CDCConsumer* cdc_consumer_;

  std::atomic<bool> is_polling_{true};
  int poll_failures_ GUARDED_BY(data_mutex_){0};
  int apply_failures_ GUARDED_BY(data_mutex_){0};
  int idle_polls_ GUARDED_BY(data_mutex_){0};
};

} // namespace enterprise
} // namespace tserver
} // namespace yb

#endif // ENT_SRC_YB_TSERVER_CDC_POLLER_H
