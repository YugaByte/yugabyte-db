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

#ifndef YB_TABLET_TRANSACTION_STATUS_RESOLVER_H
#define YB_TABLET_TRANSACTION_STATUS_RESOLVER_H

#include <memory>

#include "yb/rpc/rpc_fwd.h"

#include "yb/tablet/transaction_participant.h"

#include "yb/util/status.h"

namespace yb {
namespace tablet {

struct TransactionStatusInfo {
  TransactionId transaction_id;
  TransactionStatus status;
  HybridTime status_ht;

  std::string ToString() const {
    return Format("{ transaction_id: $0 status: $1 status_ht: $2 }",
                  transaction_id, status, status_ht);
  }
};

using TransactionStatusResolverCallback =
    std::function<void(const std::vector<TransactionStatusInfo>&)>;

// Utility class to resolve status of multiple transactions.
// It sends one request at a time to avoid generating too much load for transaction status
// resolution.
class TransactionStatusResolver {
 public:
  // If max_transactions_per_request is zero then resolution is skipped.
  TransactionStatusResolver(
      TransactionParticipantContext* participant_context, rpc::Rpcs* rpcs,
      size_t max_transactions_per_request,
      TransactionStatusResolverCallback callback);
  ~TransactionStatusResolver();

  // Shutdown this resolver.
  void Shutdown();

  // Add transaction id with its status tablet to the set of transactions to resolve.
  // Cannot be called after Start.
  void Add(const TabletId& status_tablet, const TransactionId& transaction_id);

  // Starts transaction resolution, no more adds are allowed after this point.
  void Start(CoarseTimePoint deadline);

  // Returns future for resolution status.
  std::future<Status> ResultFuture();

  bool Running() const;

 private:
  class Impl;

  std::unique_ptr<Impl> impl_;
};

} // namespace tablet
} // namespace yb

#endif // YB_TABLET_TRANSACTION_STATUS_RESOLVER_H
