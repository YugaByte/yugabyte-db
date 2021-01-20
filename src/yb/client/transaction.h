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

#ifndef YB_CLIENT_TRANSACTION_H
#define YB_CLIENT_TRANSACTION_H

#include <future>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "yb/common/common.pb.h"
#include "yb/common/consistent_read_point.h"
#include "yb/common/read_hybrid_time.h"
#include "yb/common/transaction.h"

#include "yb/client/client_fwd.h"

#include "yb/util/async_util.h"
#include "yb/util/status.h"

namespace yb {

class HybridTime;

namespace client {

struct InFlightOpsGroup {
  using Iterator = internal::InFlightOps::const_iterator;

  bool need_metadata = false;
  const Iterator begin;
  const Iterator end;

  InFlightOpsGroup(const Iterator& group_begin, const Iterator& group_end);
  std::string ToString() const;
};

struct InFlightOpsGroupsWithMetadata {
  static const size_t kPreallocatedCapacity = 40;

  boost::container::small_vector<InFlightOpsGroup, kPreallocatedCapacity> groups;
  TransactionMetadata metadata;
};

typedef StatusFunctor Waiter;
typedef StatusFunctor CommitCallback;
typedef std::function<void(const Result<ChildTransactionDataPB>&)> PrepareChildCallback;

struct ChildTransactionData {
  TransactionMetadata metadata;
  ReadHybridTime read_time;
  ConsistentReadPoint::HybridTimeMap local_limits;

  static Result<ChildTransactionData> FromPB(const ChildTransactionDataPB& data);
};

// SealOnly is a special commit mode.
// I.e. sealed transaction will be committed after seal record and all write batches are replicated.
YB_STRONGLY_TYPED_BOOL(SealOnly);

// YBTransaction is a representation of a single transaction.
// After YBTransaction is created, it could be used during construction of YBSession,
// to indicate that this session will send commands related to this transaction.
class YBTransaction : public std::enable_shared_from_this<YBTransaction> {
 private:
  class PrivateOnlyTag {};

 public:
  explicit YBTransaction(TransactionManager* manager);

  // Trick to allow std::make_shared with this ctor only from methods of this class.
  YBTransaction(TransactionManager* manager, const TransactionMetadata& metadata, PrivateOnlyTag);

  // Creates "child" transaction.
  // Child transaction shares same metadata as parent transaction, so all writes are done
  // as part of parent transaction.
  // But lifetime is controlled by parent transaction.
  // I.e. only parent transaction could be committed or aborted, also only parent transaction
  // sends heartbeats.
  YBTransaction(TransactionManager* manager, ChildTransactionData data);

  ~YBTransaction();

  void SetPriority(uint64_t priority);

  uint64_t GetPriority() const;

  // Should be invoked to complete transaction creation.
  // Transaction is unusable before Init is called.
  CHECKED_STATUS Init(
      IsolationLevel isolation, const ReadHybridTime& read_time = ReadHybridTime());

  // Allows starting a transaction that reuses an existing read point.
  void InitWithReadPoint(IsolationLevel isolation, ConsistentReadPoint&& read_point);

  // This function is used to init metadata of Write/Read request.
  // If we don't have enough information, then the function returns false and stores
  // the waiter, which will be invoked when we obtain such information.
  bool Prepare(InFlightOpsGroupsWithMetadata* ops_info,
               ForceConsistentRead force_consistent_read,
               CoarseTimePoint deadline,
               Initial initial,
               Waiter waiter);

  // Ask transaction to expect `count` operations in future. I.e. Prepare will be called with such
  // number of ops.
  void ExpectOperations(size_t count);

  // Notifies transaction that specified ops were flushed with some status.
  void Flushed(
      const internal::InFlightOps& ops, const ReadHybridTime& used_read_time, const Status& status);

  // Commits this transaction.
  void Commit(CoarseTimePoint deadline, SealOnly seal_only, CommitCallback callback);

  void Commit(CoarseTimePoint deadline, CommitCallback callback) {
    Commit(deadline, SealOnly::kFalse, callback);
  }

  void Commit(CommitCallback callback) {
    Commit(CoarseTimePoint(), SealOnly::kFalse, std::move(callback));
  }

  // Utility function for Commit.
  std::future<Status> CommitFuture(
      CoarseTimePoint deadline = CoarseTimePoint(), SealOnly seal_only = SealOnly::kFalse);

  // Aborts this transaction.
  void Abort(CoarseTimePoint deadline = CoarseTimePoint());

  // Returns transaction ID.
  const TransactionId& id() const;

  const ConsistentReadPoint& read_point() const;
  ConsistentReadPoint& read_point();

  bool IsRestartRequired() const;

  // Creates restarted transaction, this transaction should be in the "restart required" state.
  Result<YBTransactionPtr> CreateRestartedTransaction();

  // Setup precreated transaction to be restarted version of this transaction.
  CHECKED_STATUS FillRestartedTransaction(const YBTransactionPtr& dest);

  // Prepares child data, so child transaction could be started in another server.
  // Should be async because status tablet could be not ready yet.
  void PrepareChild(
      ForceConsistentRead force_consistent_read, CoarseTimePoint deadline,
      PrepareChildCallback callback);

  std::future<Result<ChildTransactionDataPB>> PrepareChildFuture(
      ForceConsistentRead force_consistent_read, CoarseTimePoint deadline = CoarseTimePoint());

  // After we finish all child operations, we should finish child and send result to parent.
  Result<ChildTransactionResultPB> FinishChild();

  // Apply results from child to this parent transaction.
  // `result` should be prepared with FinishChild of child transaction.
  CHECKED_STATUS ApplyChildResult(const ChildTransactionResultPB& result);

  std::shared_future<Result<TransactionMetadata>> GetMetadata() const;

  std::string ToString() const;

  const IsolationLevel isolation() const;

  // Releases this transaction object returning its metadata.
  // So this transaction could be used by some other application instance.
  Result<TransactionMetadata> Release();

  // Creates transaction by metadata, could be used in pair with release to transfer transaction
  // between application instances.
  static YBTransactionPtr Take(TransactionManager* manager, const TransactionMetadata& metadata);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace client
} // namespace yb

#endif // YB_CLIENT_TRANSACTION_H
