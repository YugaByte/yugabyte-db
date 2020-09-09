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

#include "yb/tserver/cdc_poller.h"
#include "yb/tserver/cdc_consumer.h"
#include "yb/tserver/twodc_output_client.h"

#include "yb/cdc/cdc_rpc.h"
#include "yb/cdc/cdc_service.pb.h"
#include "yb/cdc/cdc_service.proxy.h"
#include "yb/client/client.h"

#include "yb/consensus/opid_util.h"
#include "yb/util/logging.h"
#include "yb/util/threadpool.h"

// Similar heuristic to heartbeat_interval in heartbeater.cc.
DEFINE_int32(async_replication_polling_delay_ms, 0,
             "How long to delay in ms between applying and polling.");
DEFINE_int32(async_replication_idle_delay_ms, 100,
             "How long to delay between polling when we expect no data at the destination.");
DEFINE_int32(async_replication_max_idle_wait, 3,
             "Maximum number of consecutive empty GetChanges until the poller "
             "backs off to the idle interval, rather than immediately retrying.");
DEFINE_int32(replication_failure_delay_exponent, 16 /* ~ 2^16/1000 ~= 65 sec */,
             "Max number of failures (N) to use when calculating exponential backoff (2^N-1).");
DEFINE_bool(cdc_consumer_use_proxy_forwarding, false,
            "When enabled, read requests from the CDC Consumer that go to the wrong node are "
            "forwarded to the correct node by the Producer.");

DECLARE_int32(cdc_read_rpc_timeout_ms);

namespace yb {
namespace tserver {
namespace enterprise {

CDCPoller::CDCPoller(const cdc::ProducerTabletInfo& producer_tablet_info,
                     const cdc::ConsumerTabletInfo& consumer_tablet_info,
                     std::function<bool(void)> should_continue_polling,
                     std::function<void(void)> remove_self_from_pollers_map,
                     ThreadPool* thread_pool,
                     const std::shared_ptr<CDCClient>& local_client,
                     const std::shared_ptr<CDCClient>& producer_client,
                     CDCConsumer* cdc_consumer,
                     bool use_local_tserver) :
    producer_tablet_info_(producer_tablet_info),
    consumer_tablet_info_(consumer_tablet_info),
    should_continue_polling_(std::move(should_continue_polling)),
    remove_self_from_pollers_map_(std::move(remove_self_from_pollers_map)),
    op_id_(consensus::MinimumOpId()),
    resp_(std::make_unique<cdc::GetChangesResponsePB>()),
    output_client_(CreateTwoDCOutputClient(
        cdc_consumer,
        consumer_tablet_info,
        local_client,
        std::bind(&CDCPoller::HandleApplyChanges, this, std::placeholders::_1),
        use_local_tserver)),
    producer_client_(producer_client),
    thread_pool_(thread_pool),
    cdc_consumer_(cdc_consumer) {}

std::string CDCPoller::LogPrefixUnlocked() const {
  return strings::Substitute("P [$0:$1] C [$2:$3]: ",
                             producer_tablet_info_.stream_id,
                             producer_tablet_info_.tablet_id,
                             consumer_tablet_info_.table_id,
                             consumer_tablet_info_.tablet_id);
}

bool CDCPoller::CheckOnline() {
  return cdc_consumer_ != nullptr;
}

#define RETURN_WHEN_OFFLINE() \
  if (!CheckOnline()) { \
    LOG_WITH_PREFIX_UNLOCKED(WARNING) << "CDC Poller went offline"; \
    return; \
  }

void CDCPoller::Poll() {
  RETURN_WHEN_OFFLINE();
  WARN_NOT_OK(thread_pool_->SubmitFunc(std::bind(&CDCPoller::DoPoll, this)),
              "Could not submit Poll to thread pool");
}

void CDCPoller::DoPoll() {
  RETURN_WHEN_OFFLINE();

  auto retained = shared_from_this();
  std::lock_guard<std::mutex> l(data_mutex_);

  // determine if we should delay our upcoming poll
  int64_t delay = FLAGS_async_replication_polling_delay_ms; // normal throttling.
  if (idle_polls_ >= FLAGS_async_replication_max_idle_wait) {
    delay = max(delay, (int64_t)FLAGS_async_replication_idle_delay_ms); // idle backoff.
  }
  if (poll_failures_ > 0) {
    delay = max(delay, (int64_t)1 << poll_failures_); // exponential backoff for failures.
  }
  if (delay > 0) {
    SleepFor(MonoDelta::FromMilliseconds(delay));
  }

  cdc::GetChangesRequestPB req;
  req.set_stream_id(producer_tablet_info_.stream_id);
  req.set_tablet_id(producer_tablet_info_.tablet_id);
  req.set_serve_as_proxy(FLAGS_cdc_consumer_use_proxy_forwarding);

  cdc::CDCCheckpointPB checkpoint;
  *checkpoint.mutable_op_id() = op_id_;
  if (checkpoint.op_id().index() > 0 || checkpoint.op_id().term() > 0) {
    // Only send non-zero checkpoints in request.
    // If we don't know the latest checkpoint, then CDC producer can use the checkpoint from
    // cdc_state table.
    // This is useful in scenarios where a new tablet peer becomes replication leader for a
    // producer tablet and is not aware of the last checkpoint.
    *req.mutable_from_checkpoint() = checkpoint;
  }

  auto rpcs = producer_client_->rpcs;
  auto read_rpc_handle = rpcs->Prepare();
  if (read_rpc_handle != rpcs->InvalidHandle()) {
    *read_rpc_handle = CreateGetChangesCDCRpc(
        CoarseMonoClock::now() + MonoDelta::FromMilliseconds(FLAGS_cdc_read_rpc_timeout_ms),
        nullptr, /* RemoteTablet: will get this from 'req' */
        producer_client_->client.get(),
        &req,
        [=](const Status &status, cdc::GetChangesResponsePB &&new_resp) {
          auto retained = rpcs->Unregister(read_rpc_handle);
          auto resp = std::make_shared<cdc::GetChangesResponsePB>(std::move(new_resp));
          WARN_NOT_OK(thread_pool_->SubmitFunc(std::bind(&CDCPoller::HandlePoll, this,
                                                         status, resp)),
                      "Could not submit HandlePoll to thread pool");
        });
    (**read_rpc_handle).SendRpc();
  } else {
    // Handle the Poll as a failure so repeated invocations will incur backoff.
    WARN_NOT_OK(thread_pool_->SubmitFunc(std::bind(&CDCPoller::HandlePoll, this,
                  STATUS(Aborted, LogPrefixUnlocked() + "InvalidHandle for GetChangesCDCRpc"),
                  resp_)),
                "Could not submit HandlePoll to thread pool");
  }
}

void CDCPoller::HandlePoll(yb::Status status,
                           std::shared_ptr<cdc::GetChangesResponsePB> resp) {
  RETURN_WHEN_OFFLINE();

  auto retained = shared_from_this();
  std::lock_guard<std::mutex> l(data_mutex_);

  if (!should_continue_polling_()) {
    return remove_self_from_pollers_map_();
  }

  status_ = status;
  resp_ = resp;

  bool failed = false;
  if (!status_.ok()) {
    LOG_WITH_PREFIX_UNLOCKED(INFO) << "CDCPoller failure: " << status_.ToString();
    failed = true;
  } else if (resp_->has_error()) {
    LOG_WITH_PREFIX_UNLOCKED(WARNING) << "CDCPoller failure response: code="
                                      << resp_->error().code()
                                      << ", status=" << resp->error().status().DebugString();
    failed = true;
  } else if (!resp_->has_checkpoint()) {
    LOG_WITH_PREFIX_UNLOCKED(ERROR) << "CDCPoller failure: no checkpoint";
    failed = true;
  }
  if (failed) {
    // In case of errors, try polling again with backoff
    poll_failures_ = min(poll_failures_ + 1, FLAGS_replication_failure_delay_exponent);
    return Poll();
  }
  poll_failures_ = max(poll_failures_ - 2, 0); // otherwise, recover slowly if we're congested

  // Success Case: ApplyChanges() from Poll
  WARN_NOT_OK(output_client_->ApplyChanges(resp_.get()), "Could not ApplyChanges");
}

void CDCPoller::HandleApplyChanges(cdc::OutputClientResponse response) {
  RETURN_WHEN_OFFLINE();

  WARN_NOT_OK(thread_pool_->SubmitFunc(std::bind(&CDCPoller::DoHandleApplyChanges, this, response)),
              "Could not submit HandleApplyChanges to thread pool");
}

void CDCPoller::DoHandleApplyChanges(cdc::OutputClientResponse response) {
  RETURN_WHEN_OFFLINE();

  auto retained = shared_from_this();
  std::lock_guard<std::mutex> l(data_mutex_);

  if (!should_continue_polling_()) {
    return remove_self_from_pollers_map_();
  }

  if (!response.status.ok()) {
    LOG_WITH_PREFIX_UNLOCKED(WARNING) << "ApplyChanges failure: " << response.status;
    // Repeat the ApplyChanges step, with exponential backoff
    apply_failures_ = min(apply_failures_ + 1, FLAGS_replication_failure_delay_exponent);
    int64_t delay = (1 << apply_failures_) -1;
    SleepFor(MonoDelta::FromMilliseconds(delay));
    WARN_NOT_OK(output_client_->ApplyChanges(resp_.get()), "Could not ApplyChanges");
    return;
  }
  apply_failures_ = max(apply_failures_ - 2, 0); // recover slowly if we've gotten congested

  op_id_ = response.last_applied_op_id;

  idle_polls_ = (response.processed_record_count == 0) ? idle_polls_ + 1 : 0;

  Poll();
}
#undef RETURN_WHEN_OFFLINE

} // namespace enterprise
} // namespace tserver
} // namespace yb
