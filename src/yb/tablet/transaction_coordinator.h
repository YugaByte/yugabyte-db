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

#ifndef YB_TABLET_TRANSACTION_COORDINATOR_H
#define YB_TABLET_TRANSACTION_COORDINATOR_H

#include <future>
#include <memory>

#include "yb/client/client_fwd.h"

#include "yb/common/hybrid_time.h"
#include "yb/common/transaction.h"

#include "yb/consensus/consensus_fwd.h"
#include "yb/consensus/opid_util.h"

#include "yb/gutil/ref_counted.h"

#include "yb/rpc/rpc_fwd.h"

#include "yb/server/server_fwd.h"

#include "yb/util/enums.h"
#include "yb/util/metrics.h"
#include "yb/util/opid.h"
#include "yb/util/status.h"

namespace yb {

namespace server {

class Clock;

}

namespace tserver {

class AbortTransactionResponsePB;
class GetTransactionStatusResponsePB;
class TransactionStatePB;

}

namespace tablet {

class TransactionIntentApplier;
class UpdateTxnOperationState;

// Get current transaction timeout.
std::chrono::microseconds GetTransactionTimeout();

// Context for transaction coordinator. I.e. access to external facilities required by
// transaction coordinator to do its job.
class TransactionCoordinatorContext {
 public:
  virtual const std::string& tablet_id() const = 0;
  virtual const std::shared_future<client::YBClient*>& client_future() const = 0;
  virtual int64_t LeaderTerm() const = 0;
  virtual const server::ClockPtr& clock_ptr() const = 0;

  // Returns current hybrid time lease expiration.
  // Valid only if we are leader.
  virtual HybridTime HtLeaseExpiration() const = 0;

  virtual void UpdateClock(HybridTime hybrid_time) = 0;
  virtual std::unique_ptr<UpdateTxnOperationState> CreateUpdateTransactionState(
      tserver::TransactionStatePB* request) = 0;
  virtual void SubmitUpdateTransaction(
      std::unique_ptr<UpdateTxnOperationState> state, int64_t term) = 0;

  server::Clock& clock() const {
    return *clock_ptr();
  }

 protected:
  ~TransactionCoordinatorContext() {}
};

typedef std::function<void(Result<TransactionStatusResult>)> TransactionAbortCallback;

// Coordinates all transactions managed by specific tablet, i.e. all transactions
// that selected this tablet as status tablet for it.
// Also it handles running transactions, i.e. transactions that has intents in appropriate tablet.
// Each tablet has separate transaction coordinator.
class TransactionCoordinator {
 public:
  TransactionCoordinator(const std::string& permanent_uuid,
                         TransactionCoordinatorContext* context,
                         Counter* expired_metric);
  ~TransactionCoordinator();

  // Used to pass arguments to ProcessReplicated.
  struct ReplicatedData {
    int64_t leader_term;
    const tserver::TransactionStatePB& state;
    const OpIdPB& op_id;
    HybridTime hybrid_time;

    std::string ToString() const;
  };

  // Process new transaction state.
  CHECKED_STATUS ProcessReplicated(const ReplicatedData& data);

  struct AbortedData {
    const tserver::TransactionStatePB& state;
    const OpIdPB& op_id;
  };

  // Process transaction state replication aborted.
  void ProcessAborted(const AbortedData& data);

  // Handles new request for transaction update.
  void Handle(std::unique_ptr<tablet::UpdateTxnOperationState> request, int64_t term);

  // Prepares log garbage collection. Return min index that should be preserved.
  int64_t PrepareGC(std::string* details = nullptr);

  // Starts background processes of transaction coordinator.
  void Start();

  // Stop background processes of transaction coordinator.
  // And like most of other Shutdowns in our codebase it wait until shutdown completes.
  void Shutdown();

  CHECKED_STATUS GetStatus(const google::protobuf::RepeatedPtrField<std::string>& transaction_ids,
                           CoarseTimePoint deadline,
                           tserver::GetTransactionStatusResponsePB* response);

  void Abort(const std::string& transaction_id, int64_t term, TransactionAbortCallback callback);

  std::string DumpTransactions();

  // Returns count of managed transactions. Used in tests.
  size_t test_count_transactions() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace tablet
} // namespace yb

#endif // YB_TABLET_TRANSACTION_COORDINATOR_H
