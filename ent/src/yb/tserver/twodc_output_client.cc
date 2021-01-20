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

#include "yb/tserver/twodc_output_client.h"

#include <shared_mutex>

#include "yb/cdc/cdc_util.h"
#include "yb/cdc/cdc_rpc.h"
#include "yb/client/client.h"
#include "yb/client/client_utils.h"
#include "yb/client/meta_cache.h"
#include "yb/gutil/strings/join.h"
#include "yb/rpc/rpc.h"
#include "yb/rpc/rpc_fwd.h"
#include "yb/tserver/cdc_consumer.h"
#include "yb/tserver/tserver_service.proxy.h"
#include "yb/tserver/twodc_write_interface.h"
#include "yb/util/flag_tags.h"
#include "yb/util/logging.h"
#include "yb/util/net/net_util.h"

DECLARE_int32(cdc_write_rpc_timeout_ms);

DEFINE_bool(cdc_force_remote_tserver, false,
            "Avoid local tserver apply optimization for CDC and force remote RPCs.");
TAG_FLAG(cdc_force_remote_tserver, runtime);

DECLARE_int32(cdc_read_rpc_timeout_ms);

using namespace std::placeholders;

namespace yb {
namespace tserver {
namespace enterprise {

using rpc::Rpc;

class TwoDCOutputClient : public cdc::CDCOutputClient {
 public:
  TwoDCOutputClient(
      CDCConsumer* cdc_consumer,
      const cdc::ConsumerTabletInfo& consumer_tablet_info,
      const std::shared_ptr<CDCClient>& local_client,
      rpc::Rpcs* rpcs,
      std::function<void(const cdc::OutputClientResponse& response)> apply_changes_clbk,
      bool use_local_tserver) :
      cdc_consumer_(cdc_consumer),
      consumer_tablet_info_(consumer_tablet_info),
      local_client_(local_client),
      rpcs_(rpcs),
      write_handle_(rpcs->InvalidHandle()),
      apply_changes_clbk_(std::move(apply_changes_clbk)),
      use_local_tserver_(use_local_tserver) {}

  ~TwoDCOutputClient() {
    std::lock_guard<decltype(lock_)> l(lock_);
    rpcs_->Abort({&write_handle_});
  }

  CHECKED_STATUS ApplyChanges(const cdc::GetChangesResponsePB* resp) override;

  void WriteCDCRecordDone(const Status& status, const WriteResponsePB& response);

 private:
  void TabletLookupCallback(
      const size_t record_idx, const Result<client::internal::RemoteTabletPtr>& tablet);

  void TabletLookupCallbackFastTrack(const size_t record_idx);

  // Processes the Record and sends the CDCWrite for it.
  void ProcessRecord(const std::vector<std::string>& tablet_ids, const cdc::CDCRecordPB& record);

  void SendNextCDCWriteToTablet(std::unique_ptr<WriteRequestPB> write_request);

  // Increment processed record count.
  // Returns true if all records are processed, false if there are still some pending records.
  bool IncProcessedRecordCount() REQUIRES(lock_);

  cdc::OutputClientResponse PrepareResponse() REQUIRES(lock_);
  void SendResponse(const cdc::OutputClientResponse& resp) EXCLUDES(lock_);

  void HandleResponse() EXCLUDES(lock_);
  void HandleError(const Status& s, bool done) EXCLUDES(lock_);

  bool UseLocalTserver();

  void TabletRangeLookupCallback(
      const size_t record_idx,
      const std::string partition_key_start,
      const std::string partition_key_end,
      const Result<std::vector<client::internal::RemoteTabletPtr>>& tablets);

  CDCConsumer* cdc_consumer_;
  cdc::ConsumerTabletInfo consumer_tablet_info_;
  std::shared_ptr<CDCClient> local_client_;
  rpc::Rpcs* rpcs_;
  rpc::Rpcs::Handle write_handle_ GUARDED_BY(lock_);
  std::function<void(const cdc::OutputClientResponse& response)> apply_changes_clbk_;

  bool use_local_tserver_;

  std::shared_ptr<client::YBTable> table_;

  // Used to protect error_status_, op_id_, done_processing_, write_handle_ and record counts.
  mutable rw_spinlock lock_;
  Status error_status_ GUARDED_BY(lock_);
  OpIdPB op_id_ GUARDED_BY(lock_) = consensus::MinimumOpId();
  bool done_processing_ GUARDED_BY(lock_) = false;

  uint32_t processed_record_count_ GUARDED_BY(lock_) = 0;
  uint32_t record_count_ GUARDED_BY(lock_) = 0;

  // This will cache the response to an ApplyChanges() request.
  cdc::GetChangesResponsePB twodc_resp_copy_;

  std::unique_ptr<TwoDCWriteInterface> write_strategy_ GUARDED_BY(lock_);
};

#define INCREMENT_AND_RETURN_IF_ERROR_RESULT(result) { \
  if (!result.ok()) { \
    bool done = false; \
    { \
      std::lock_guard<decltype(lock_)> l(lock_); \
      done = IncProcessedRecordCount(); \
    } \
    HandleError(result.status(), done); \
    return; \
  } \
}

Status TwoDCOutputClient::ApplyChanges(const cdc::GetChangesResponsePB* poller_resp) {
  // ApplyChanges is called in a single threaded manner.
  // For all the changes in GetChangesResponsePB, we first fan out and find the tablet for
  // every record key.
  // Then we apply the records in the same order in which we received them.
  // Once all changes have been applied (successfully or not), we invoke the callback which will
  // then either poll for next set of changes (in case of successful application) or will try to
  // re-apply.
  DCHECK(poller_resp->has_checkpoint());
  twodc_resp_copy_.Clear();

  // Init class variables that threads will use.
  {
    std::lock_guard<decltype(lock_)> l(lock_);
    DCHECK(consensus::OpIdEquals(op_id_, consensus::MinimumOpId()));
    op_id_ = poller_resp->checkpoint().op_id();
    error_status_ = Status::OK();
    done_processing_ = false;
    processed_record_count_ = 0;
    record_count_ = poller_resp->records_size();
    ResetWriteInterface(&write_strategy_);
  }

  // Ensure we have records.
  if (poller_resp->records_size() == 0) {
    HandleResponse();
    return Status::OK();
  }

  // Ensure we have a connection to the consumer table cached.
  if (!table_) {
    Status s = local_client_->client->OpenTable(consumer_tablet_info_.table_id, &table_);
    if (!s.ok()) {
      HandleError(s, true);
      return s;
    }
  }

  twodc_resp_copy_ = *poller_resp;

  for (int i = 0; i < twodc_resp_copy_.records_size(); i++) {
    // All KV-pairs within a single CDC record will be for the same row.
    // key(0).key() will contain the hash code for that row. We use this to lookup the tablet.
    if (UseLocalTserver()) {
      TabletLookupCallbackFastTrack(i);
    } else {
      const auto& record = twodc_resp_copy_.records(i);
      if (record.operation() == cdc::CDCRecordPB::APPLY) {
        local_client_->client->LookupAllTablets(
          table_,
          CoarseMonoClock::now() + MonoDelta::FromMilliseconds(FLAGS_cdc_read_rpc_timeout_ms),
          std::bind(&TwoDCOutputClient::TabletRangeLookupCallback, this, i,
                    record.partition().partition_key_start(),
                    record.partition().partition_key_end(), std::placeholders::_1));
      } else {
        local_client_->client->LookupTabletByKey(
            table_,
            PartitionSchema::EncodeMultiColumnHashValue(
                boost::lexical_cast<uint16_t>(record.key(0).key())),
            CoarseMonoClock::now() + MonoDelta::FromMilliseconds(FLAGS_cdc_read_rpc_timeout_ms),
            std::bind(&TwoDCOutputClient::TabletLookupCallback, this, i, std::placeholders::_1));
      }
    }
  }

  if (twodc_resp_copy_.records_size() == 0) {
    // Nothing to process, return success.
    HandleResponse();
  }
  return Status::OK();
}

bool TwoDCOutputClient::UseLocalTserver() {
  return use_local_tserver_ && !FLAGS_cdc_force_remote_tserver;
}

void TwoDCOutputClient::ProcessRecord(const std::vector<std::string>& tablet_ids,
                                      const cdc::CDCRecordPB& record) {
  std::unique_ptr<WriteRequestPB> write_request;
  {
    std::lock_guard<decltype(lock_)> l(lock_);
    for (const auto& tablet_id : tablet_ids) {
      auto status = write_strategy_->ProcessRecord(tablet_id, record);
      if (!status.ok()) {
        error_status_ = status;
        return;
      }
    }
    if (!IncProcessedRecordCount()) {
      return;
    }
    if (error_status_.ok()) {
      write_request = write_strategy_->GetNextWriteRequest();
    }
  }
  // Found tablets for all records, now we should write the records.
  if (write_request) {
    // Apply the writes on consumer.
    SendNextCDCWriteToTablet(std::move(write_request));
  } else {
    // No write_request on error. Respond, without applying records.
    HandleResponse();
  }
}

void TwoDCOutputClient::TabletLookupCallback(
    const size_t record_idx,
    const Result<client::internal::RemoteTabletPtr>& tablet) {
  INCREMENT_AND_RETURN_IF_ERROR_RESULT(tablet);
  ProcessRecord({tablet->get()->tablet_id()}, twodc_resp_copy_.records(record_idx));
}

void TwoDCOutputClient::TabletRangeLookupCallback(
    const size_t record_idx,
    const std::string partition_key_start,
    const std::string partition_key_end,
    const Result<std::vector<client::internal::RemoteTabletPtr>>& tablets) {
  INCREMENT_AND_RETURN_IF_ERROR_RESULT(tablets);

  auto filtered_tablets_result = client::FilterTabletsByHashPartitionKeyRange(
      *tablets, partition_key_start, partition_key_end);
  INCREMENT_AND_RETURN_IF_ERROR_RESULT(filtered_tablets_result);

  auto filtered_tablets = *filtered_tablets_result;
  auto tablet_ids = std::vector<std::string>(filtered_tablets.size());
  std::transform(filtered_tablets.begin(), filtered_tablets.end(), tablet_ids.begin(),
                 [&](const auto& tablet_ptr) {
    return tablet_ptr->tablet_id();
  });
  ProcessRecord(tablet_ids, twodc_resp_copy_.records(record_idx));
}

void TwoDCOutputClient::TabletLookupCallbackFastTrack(const size_t record_idx) {
  ProcessRecord({consumer_tablet_info_.tablet_id}, twodc_resp_copy_.records(record_idx));
}

void TwoDCOutputClient::SendNextCDCWriteToTablet(std::unique_ptr<WriteRequestPB> write_request) {
  // TODO: This should be parallelized for better performance with M:N setups.
  auto deadline = CoarseMonoClock::Now() +
                  MonoDelta::FromMilliseconds(FLAGS_cdc_write_rpc_timeout_ms);
  std::lock_guard<decltype(lock_)> l(lock_);
  write_handle_ = rpcs_->Prepare();
  if (write_handle_ != rpcs_->InvalidHandle()) {
    // Send in nullptr for RemoteTablet since cdc rpc now gets the tablet_id from the write request.
    *write_handle_ = CreateCDCWriteRpc(
        deadline,
        nullptr /* RemoteTablet */,
        local_client_->client.get(),
        write_request.get(),
        std::bind(&TwoDCOutputClient::WriteCDCRecordDone, this, _1, _2),
        UseLocalTserver());
    (**write_handle_).SendRpc();
  } else {
    LOG(WARNING) << "Invalid handle for CDC write, tablet ID: " << write_request->tablet_id();
  }
}

void TwoDCOutputClient::WriteCDCRecordDone(const Status& status, const WriteResponsePB& response) {
  // Handle response.
  rpc::RpcCommandPtr retained = nullptr;
  {
    std::lock_guard<decltype(lock_)> l(lock_);
    retained = rpcs_->Unregister(&write_handle_);
  }
  if (!status.ok()) {
    HandleError(status, true /* done */);
    return;
  } else if (response.has_error()) {
    HandleError(StatusFromPB(response.error().status()), true /* done */);
    return;
  }
  cdc_consumer_->IncrementNumSuccessfulWriteRpcs();

  // See if we need to handle any more writes.
  std::unique_ptr <WriteRequestPB> write_request;
  {
    std::lock_guard<decltype(lock_)> l(lock_);
    write_request = write_strategy_->GetNextWriteRequest();
  }

  if (write_request) {
    SendNextCDCWriteToTablet(std::move(write_request));
  } else {
    // Last record, return response to caller.
    HandleResponse();
  }
}

void TwoDCOutputClient::HandleError(const Status& s, bool done) {
  LOG(ERROR) << "Error while applying replicated record: " << s
             << ", consumer tablet: " << consumer_tablet_info_.tablet_id;
  {
    std::lock_guard<decltype(lock_)> l(lock_);
    error_status_ = s;
  }
  if (done) {
    HandleResponse();
  }
}

cdc::OutputClientResponse TwoDCOutputClient::PrepareResponse() {
  cdc::OutputClientResponse response;
  response.status = error_status_;
  if (response.status.ok()) {
    response.last_applied_op_id = op_id_;
    response.processed_record_count = processed_record_count_;
  }
  op_id_ = consensus::MinimumOpId();
  processed_record_count_ = 0;
  return response;
}

void TwoDCOutputClient::SendResponse(const cdc::OutputClientResponse& resp) {
  apply_changes_clbk_(resp);
}

void TwoDCOutputClient::HandleResponse() {
  cdc::OutputClientResponse response;
  {
    std::lock_guard<decltype(lock_)> l(lock_);
    response = PrepareResponse();
  }
  SendResponse(response);
}

bool TwoDCOutputClient::IncProcessedRecordCount() {
  processed_record_count_++;
  if (processed_record_count_ == record_count_) {
    done_processing_ = true;
  }
  CHECK(processed_record_count_ <= record_count_);
  return done_processing_;
}

std::unique_ptr<cdc::CDCOutputClient> CreateTwoDCOutputClient(
    CDCConsumer* cdc_consumer,
    const cdc::ConsumerTabletInfo& consumer_tablet_info,
    const std::shared_ptr<CDCClient>& local_client,
    rpc::Rpcs* rpcs,
    std::function<void(const cdc::OutputClientResponse& response)> apply_changes_clbk,
    bool use_local_tserver) {
  return std::make_unique<TwoDCOutputClient>(cdc_consumer, consumer_tablet_info, local_client, rpcs,
                                             std::move(apply_changes_clbk), use_local_tserver);
}

} // namespace enterprise
} // namespace tserver
} // namespace yb
