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

#include "yb/client/transaction_cleanup.h"

#include "yb/client/client.h"
#include "yb/client/meta_cache.h"

#include "yb/tserver/tserver_service.proxy.h"

using namespace std::literals;
using namespace std::placeholders;

namespace yb {
namespace client {

namespace {

class TransactionCleanup : public std::enable_shared_from_this<TransactionCleanup> {
 public:
  TransactionCleanup(
      YBClient* client, const scoped_refptr<ClockBase>& clock, const TransactionId& transaction_id,
      Sealed sealed, CleanupType type)
      : client_(client), clock_(clock), transaction_id_(transaction_id), sealed_(sealed),
        type_(type) {
  }

  void Perform(const std::vector<TabletId>& tablet_ids) {
    auto self = shared_from_this();
    for (const auto& tablet_id : tablet_ids) {
      // TODO(tsplit): pass table if needed as a part of
      // https://github.com/yugabyte/yugabyte-db/issues/4942.
      client_->LookupTabletById(
          tablet_id,
          /* table =*/ nullptr,
          TransactionRpcDeadline(),
          std::bind(&TransactionCleanup::LookupTabletDone, this, _1, self),
          client::UseCache::kTrue);
    }
  }

 private:
  void LookupTabletDone(const Result<internal::RemoteTabletPtr>& remote_tablet,
                        const std::shared_ptr<TransactionCleanup>& self) {
    if (!remote_tablet.ok()) {
      // Intents will be cleaned up later in this case.
      LOG_WITH_PREFIX(WARNING) << "Tablet lookup failed: " << remote_tablet.status();
      return;
    }
    VLOG_WITH_PREFIX(1) << "Lookup tablet for cleanup done: " << yb::ToString(*remote_tablet);
    auto remote_tablet_servers = (**remote_tablet).GetRemoteTabletServers(
        internal::IncludeFailedReplicas::kTrue);

    constexpr auto kCallTimeout = 15s;
    auto now = clock_->Now().ToUint64();

    const auto& tablet_id = (**remote_tablet).tablet_id();

    std::lock_guard<std::mutex> lock(mutex_);
    calls_.reserve(calls_.size() + remote_tablet_servers.size());
    for (auto* server : remote_tablet_servers) {
      VLOG_WITH_PREFIX(2) << "Sending cleanup to T " << (**remote_tablet).tablet_id() << " P "
                          << server->permanent_uuid();
      auto status = server->InitProxy(client_);
      if (!status.ok()) {
        LOG_WITH_PREFIX(WARNING) << "Failed to init proxy to " << server->ToString() << ": "
                                 << status;
        continue;
      }
      calls_.emplace_back();
      auto& call = calls_.back();

      auto& request = call.request;
      request.set_tablet_id(tablet_id);
      request.set_propagated_hybrid_time(now);
      auto& state = *request.mutable_state();
      state.set_transaction_id(transaction_id_.data(), transaction_id_.size());
      state.set_status(type_ == CleanupType::kImmediate ? TransactionStatus::IMMEDIATE_CLEANUP
                                                        : TransactionStatus::GRACEFUL_CLEANUP);
      state.set_sealed(sealed_);

      call.controller.set_timeout(kCallTimeout);

      server->proxy()->UpdateTransactionAsync(
          request, &call.response, &call.controller,
          [this, self, remote_tablet = *remote_tablet, server] {
            VLOG_WITH_PREFIX(3) << "Cleaned intents at T " << remote_tablet->tablet_id() << " P "
                                << server->permanent_uuid();
          });
    }
  }

  std::string LogPrefix() const {
    return Format("ID $0: ", transaction_id_);
  }

  struct Call {
    tserver::UpdateTransactionRequestPB request;
    tserver::UpdateTransactionResponsePB response;
    rpc::RpcController controller;
  };

  YBClient* const client_;
  const scoped_refptr<ClockBase> clock_;
  const TransactionId transaction_id_;
  const Sealed sealed_;
  const CleanupType type_;

  std::mutex mutex_;
  boost::container::stable_vector<Call> calls_ GUARDED_BY(mutex_);
};

} // namespace

void CleanupTransaction(
    YBClient* client, const scoped_refptr<ClockBase>& clock, const TransactionId& transaction_id,
    Sealed sealed, CleanupType type, const std::vector<TabletId>& tablets) {
  auto cleanup = std::make_shared<TransactionCleanup>(
      client, clock, transaction_id, sealed, type);
  cleanup->Perform(tablets);
}

} // namespace client
} // namespace yb
