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

#ifndef YB_TABLET_TRANSACTION_LOADER_H
#define YB_TABLET_TRANSACTION_LOADER_H

#include <condition_variable>

#include "yb/common/transaction.h"

#include "yb/docdb/docdb.h"

namespace yb {

class OneWayBitmap;
class RWOperationCounter;

namespace tablet {

class TransactionStatusResolver;
struct TransactionalBatchData;

struct ApplyStateWithCommitHt {
  docdb::ApplyTransactionState state;
  HybridTime commit_ht;

  std::string ToString() const {
    return YB_STRUCT_TO_STRING(state, commit_ht);
  }
};

using ApplyStatesMap = std::unordered_map<
    TransactionId, ApplyStateWithCommitHt, TransactionIdHash>;

class TransactionLoaderContext {
 public:
  virtual ~TransactionLoaderContext() = default;

  virtual TransactionStatusResolver& AddStatusResolver() = 0;
  virtual const std::string& LogPrefix() const = 0;
  virtual void CompleteLoad(const std::function<void()>& functor) = 0;
  virtual void LoadTransaction(
      TransactionMetadata&& metadata,
      TransactionalBatchData&& last_batch_data,
      OneWayBitmap&& replicated_batches,
      const ApplyStateWithCommitHt* pending_apply) = 0;
  virtual void LoadFinished(const ApplyStatesMap& pending_applies) = 0;
};

class TransactionLoader {
 public:
  TransactionLoader(TransactionLoaderContext* context, const scoped_refptr<MetricEntity>& entity);
  ~TransactionLoader();

  void Start(RWOperationCounter* pending_op_counter, const docdb::DocDB& db);

  bool complete() const {
    return all_loaded_.load(std::memory_order_acquire);
  }

  void WaitLoaded(const TransactionId& id);
  void WaitAllLoaded();

  void Shutdown();

 private:
  class Executor;
  friend class Executor;

  TransactionLoaderContext& context_;
  const scoped_refptr<MetricEntity> entity_;

  std::unique_ptr<Executor> executor_;

  std::mutex mutex_;
  std::condition_variable load_cond_;
  TransactionId last_loaded_ GUARDED_BY(mutex_) = TransactionId::Nil();
  std::atomic<bool> all_loaded_{false};
  std::thread load_thread_;
};

} // namespace tablet
} // namespace yb

#endif // YB_TABLET_TRANSACTION_LOADER_H
