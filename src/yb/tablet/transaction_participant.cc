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

#include "yb/tablet/transaction_participant.h"

#include <mutex>
#include <queue>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/mem_fun.hpp>

#include <boost/optional/optional.hpp>

#include <boost/uuid/uuid_io.hpp>

#include "yb/rocksdb/write_batch.h"

#include "yb/client/transaction_rpc.h"

#include "yb/common/pgsql_error.h"

#include "yb/consensus/consensus_util.h"

#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/docdb/docdb.h"

#include "yb/rpc/poller.h"
#include "yb/rpc/rpc.h"
#include "yb/rpc/rpc_context.h"
#include "yb/rpc/thread_pool.h"

#include "yb/tablet/cleanup_aborts_task.h"
#include "yb/tablet/cleanup_intents_task.h"
#include "yb/tablet/operations/update_txn_operation.h"
#include "yb/tablet/running_transaction.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/transaction_loader.h"
#include "yb/tablet/transaction_status_resolver.h"

#include "yb/tserver/tserver_service.pb.h"
#include "yb/tserver/service_util.h"

#include "yb/util/delayer.h"
#include "yb/util/flag_tags.h"
#include "yb/util/locks.h"
#include "yb/util/lru_cache.h"
#include "yb/util/monotime.h"
#include "yb/util/pb_util.h"
#include "yb/util/random_util.h"
#include "yb/util/scope_exit.h"
#include "yb/util/thread_restrictions.h"
#include "yb/util/tsan_util.h"

using namespace std::literals;
using namespace std::placeholders;

DEFINE_uint64(transaction_min_running_check_delay_ms, 50,
              "When transaction with minimal start hybrid time is updated at transaction "
              "participant, we wait at least this number of milliseconds before checking its "
              "status at transaction coordinator. Used for the optimization that deletes "
              "provisional records RocksDB SSTable files.");

DEFINE_uint64(transaction_min_running_check_interval_ms, 250,
              "While transaction with minimal start hybrid time remains the same, we will try "
              "to check its status at transaction coordinator at regular intervals this "
              "long (ms). Used for the optimization that deletes "
              "provisional records RocksDB SSTable files.");

DEFINE_test_flag(double, transaction_ignore_applying_probability_in_tests, 0,
                 "Probability to ignore APPLYING update in tests.");
DEFINE_test_flag(bool, fail_in_apply_if_no_metadata, false,
                 "Fail when applying intents if metadata is not found.");

DEFINE_uint64(max_transactions_in_status_request, 128,
              "Request status for at most specified number of transactions at once. "
                  "0 disables load time transaction status resolution.");

DEFINE_uint64(transactions_cleanup_cache_size, 64, "Transactions cleanup cache size.");

DEFINE_uint64(transactions_status_poll_interval_ms, 500 * yb::kTimeMultiplier,
              "Transactions poll interval.");

DEFINE_bool(transactions_poll_check_aborted, true, "Check aborted transactions during poll.");

DECLARE_int64(transaction_abort_check_timeout_ms);

METRIC_DEFINE_simple_counter(
    tablet, transaction_not_found,
    "Total number of missing transactions during load",
    yb::MetricUnit::kTransactions);
METRIC_DEFINE_simple_gauge_uint64(
    tablet, transactions_running,
    "Total number of transactions running in participant",
    yb::MetricUnit::kTransactions);

namespace yb {
namespace tablet {

namespace {

YB_STRONGLY_TYPED_BOOL(PostApplyCleanup);

} // namespace

std::string TransactionApplyData::ToString() const {
  return YB_STRUCT_TO_STRING(
      leader_term, transaction_id, op_id, commit_ht, log_ht, sealed, status_tablet, apply_state);
}

class TransactionParticipant::Impl
    : public RunningTransactionContext, public TransactionLoaderContext {
 public:
  Impl(TransactionParticipantContext* context, TransactionIntentApplier* applier,
       const scoped_refptr<MetricEntity>& entity)
      : RunningTransactionContext(context, applier),
        log_prefix_(context->LogPrefix()),
        loader_(this, entity),
        poller_(log_prefix_, std::bind(&Impl::Poll, this)) {
    LOG_WITH_PREFIX(INFO) << "Create";
    metric_transactions_running_ = METRIC_transactions_running.Instantiate(entity, 0);
    metric_transaction_not_found_ = METRIC_transaction_not_found.Instantiate(entity);
  }

  ~Impl() {
    if (StartShutdown()) {
      CompleteShutdown();
    } else {
      LOG_IF_WITH_PREFIX(DFATAL, !shutdown_done_.load(std::memory_order_acquire))
          << "Destroying transaction participant that did not complete shutdown";
    }
  }

  bool StartShutdown() {
    bool expected = false;
    if (!closing_.compare_exchange_strong(expected, true)) {
      return false;
    }

    poller_.Shutdown();

    if (start_latch_.count()) {
      start_latch_.CountDown();
    }

    LOG_WITH_PREFIX(INFO) << "Shutdown";
    return true;
  }

  void CompleteShutdown() {
    LOG_IF_WITH_PREFIX(DFATAL, !closing_.load()) << __func__ << " w/o StartShutdown";

    decltype(status_resolvers_) status_resolvers;
    {
      MinRunningNotifier min_running_notifier(nullptr /* applier */);
      std::lock_guard<std::mutex> lock(mutex_);
      transactions_.clear();
      TransactionsModifiedUnlocked(&min_running_notifier);
      status_resolvers.swap(status_resolvers_);
    }

    rpcs_.Shutdown();
    loader_.Shutdown();
    for (auto& resolver : status_resolvers) {
      resolver.Shutdown();
    }
    shutdown_done_.store(true, std::memory_order_release);
  }

  bool Closing() const override {
    return closing_.load(std::memory_order_acquire);
  }

  void Start() {
    LOG_WITH_PREFIX(INFO) << "Start";
    start_latch_.CountDown();
  }

  // Adds new running transaction.
  bool Add(const TransactionMetadataPB& data, rocksdb::WriteBatch *write_batch) {
    auto metadata = TransactionMetadata::FromPB(data);
    if (!metadata.ok()) {
      LOG_WITH_PREFIX(DFATAL) << "Invalid transaction id: " << metadata.status().ToString();
      return false;
    }
    loader_.WaitLoaded(metadata->transaction_id);
    bool store = false;
    {
      MinRunningNotifier min_running_notifier(&applier_);
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = transactions_.find(metadata->transaction_id);
      if (it == transactions_.end()) {
        if (WasTransactionRecentlyRemoved(metadata->transaction_id)) {
          return false;
        }
        if (cleanup_cache_.Erase(metadata->transaction_id) != 0) {
          return false;
        }
        VLOG_WITH_PREFIX(4) << "Create new transaction: " << metadata->transaction_id;
        transactions_.insert(std::make_shared<RunningTransaction>(
            *metadata, TransactionalBatchData(), OneWayBitmap(), metadata->start_time, this));
        TransactionsModifiedUnlocked(&min_running_notifier);
        store = true;
      }
    }
    if (store) {
      docdb::KeyBytes key;
      AppendTransactionKeyPrefix(metadata->transaction_id, &key);
      auto data_copy = data;
      // We use hybrid time only for backward compatibility, actually wall time is required.
      data_copy.set_metadata_write_time(GetCurrentTimeMicros());
      auto value = data.SerializeAsString();
      write_batch->Put(key.AsSlice(), value);
    }
    return true;
  }

  HybridTime LocalCommitTime(const TransactionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = transactions_.find(id);
    if (it == transactions_.end()) {
      return HybridTime::kInvalid;
    }
    return (**it).local_commit_time();
  }

  std::pair<size_t, size_t> TEST_CountIntents() {
    {
      MinRunningNotifier min_running_notifier(&applier_);
      std::lock_guard<std::mutex> lock(mutex_);
      ProcessRemoveQueueUnlocked(&min_running_notifier);
    }

    std::pair<size_t, size_t> result(0, 0);
    auto iter = docdb::CreateRocksDBIterator(db_.intents,
                                             key_bounds_,
                                             docdb::BloomFilterMode::DONT_USE_BLOOM_FILTER,
                                             boost::none,
                                             rocksdb::kDefaultQueryId);
    for (iter.SeekToFirst(); iter.Valid(); iter.Next()) {
      ++result.first;
      // Count number of transaction, by counting metadata records.
      if (iter.key().size() == TransactionId::StaticSize() + 1) {
        ++result.second;
        auto key = iter.key();
        key.remove_prefix(1);
        auto id = CHECK_RESULT(FullyDecodeTransactionId(key));
        LOG_WITH_PREFIX(INFO) << "Stored txn meta: " << id;
      }
    }

    return result;
  }

  Result<TransactionMetadata> PrepareMetadata(const TransactionMetadataPB& pb) {
    if (pb.has_isolation()) {
      auto metadata = VERIFY_RESULT(TransactionMetadata::FromPB(pb));
      std::unique_lock<std::mutex> lock(mutex_);
      auto it = transactions_.find(metadata.transaction_id);
      if (it != transactions_.end()) {
        RETURN_NOT_OK((**it).CheckAborted());
      } else if (WasTransactionRecentlyRemoved(metadata.transaction_id)) {
        return MakeAbortedStatus(metadata.transaction_id);
      }
      return metadata;
    }

    auto id = VERIFY_RESULT(FullyDecodeTransactionId(pb.transaction_id()));

    // We are not trying to cleanup intents here because we don't know whether this transaction
    // has intents or not.
    auto lock_and_iterator = LockAndFind(
        id, "metadata"s, TransactionLoadFlags{TransactionLoadFlag::kMustExist});
    if (!lock_and_iterator.found()) {
      return STATUS(TryAgain,
                    Format("Unknown transaction, could be recently aborted: $0", id), Slice(),
                    PgsqlError(YBPgErrorCode::YB_PG_T_R_SERIALIZATION_FAILURE));
    }
    RETURN_NOT_OK(lock_and_iterator.transaction().CheckAborted());
    return lock_and_iterator.transaction().metadata();
  }

  boost::optional<std::pair<IsolationLevel, TransactionalBatchData>> PrepareBatchData(
      const TransactionId& id, size_t batch_idx,
      boost::container::small_vector_base<uint8_t>* encoded_replicated_batches) {
    // We are not trying to cleanup intents here because we don't know whether this transaction
    // has intents of not.
    auto lock_and_iterator = LockAndFind(
        id, "metadata with write id"s, TransactionLoadFlags{TransactionLoadFlag::kMustExist});
    if (!lock_and_iterator.found()) {
      return boost::none;
    }
    auto& transaction = lock_and_iterator.transaction();
    transaction.AddReplicatedBatch(batch_idx, encoded_replicated_batches);
    return std::make_pair(transaction.metadata().isolation, transaction.last_batch_data());
  }

  void BatchReplicated(const TransactionId& id, const TransactionalBatchData& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = transactions_.find(id);
    if (it == transactions_.end()) {
      LOG_IF_WITH_PREFIX(DFATAL, !WasTransactionRecentlyRemoved(id))
          << "Update last write id for unknown transaction: " << id;
      return;
    }
    (**it).BatchReplicated(data);
  }

  void RequestStatusAt(const StatusRequest& request) {
    auto lock_and_iterator = LockAndFind(*request.id, *request.reason, request.flags);
    if (!lock_and_iterator.found()) {
      request.callback(
          STATUS_FORMAT(NotFound, "Request status of unknown transaction: $0", *request.id));
      return;
    }
    lock_and_iterator.transaction().RequestStatusAt(request, &lock_and_iterator.lock);
  }

  // Registers a request, giving it a newly allocated id and returning this id.
  int64_t RegisterRequest() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = NextRequestIdUnlocked();
    running_requests_.push_back(result);
    return result;
  }

  // Unregisters a previously registered request.
  void UnregisterRequest(int64_t request) {
    MinRunningNotifier min_running_notifier(&applier_);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      DCHECK(!running_requests_.empty());
      if (running_requests_.front() != request) {
        complete_requests_.push(request);
        return;
      }
      running_requests_.pop_front();
      while (!complete_requests_.empty() && complete_requests_.top() == running_requests_.front()) {
        complete_requests_.pop();
        running_requests_.pop_front();
      }

      CleanTransactionsUnlocked(&min_running_notifier);
    }
  }

  // Cleans transactions that are requested and now is safe to clean.
  // See RemoveUnlocked for details.
  void CleanTransactionsUnlocked(MinRunningNotifier* min_running_notifier) REQUIRES(mutex_) {
    ProcessRemoveQueueUnlocked(min_running_notifier);

    CleanTransactionsQueue(&immediate_cleanup_queue_, min_running_notifier);
    CleanTransactionsQueue(&graceful_cleanup_queue_, min_running_notifier);
  }

  template <class Queue>
  void CleanTransactionsQueue(
      Queue* queue, MinRunningNotifier* min_running_notifier) REQUIRES(mutex_) {
    int64_t min_request = running_requests_.empty() ? std::numeric_limits<int64_t>::max()
                                                    : running_requests_.front();
    HybridTime safe_time;
    while (!queue->empty()) {
      const auto& front = queue->front();
      if (front.request_id >= min_request) {
        break;
      }
      if (!front.Ready(&participant_context_, &safe_time)) {
        break;
      }
      const auto& id = front.transaction_id;
      auto it = transactions_.find(id);
      if (it != transactions_.end()) {
        (**it).ScheduleRemoveIntents(*it);
        RemoveTransaction(it, min_running_notifier);
      }
      VLOG_WITH_PREFIX(2) << "Cleaned from queue: " << id;
      queue->pop_front();
    }
  }

  void Abort(const TransactionId& id, TransactionStatusCallback callback) {
    // We are not trying to cleanup intents here because we don't know whether this transaction
    // has intents of not.
    auto lock_and_iterator = LockAndFind(
        id, "abort"s, TransactionLoadFlags{TransactionLoadFlag::kMustExist});
    if (!lock_and_iterator.found()) {
      callback(STATUS_FORMAT(NotFound, "Abort of unknown transaction: $0", id));
      return;
    }
    auto client_result = client();
    if (!client_result.ok()) {
      callback(client_result.status());
      return;
    }
    lock_and_iterator.transaction().Abort(
        *client_result, std::move(callback), &lock_and_iterator.lock);
  }

  CHECKED_STATUS CheckAborted(const TransactionId& id) {
    // We are not trying to cleanup intents here because we don't know whether this transaction
    // has intents of not.
    auto lock_and_iterator = LockAndFind(id, "check aborted"s, TransactionLoadFlags{});
    if (!lock_and_iterator.found()) {
      return MakeAbortedStatus(id);
    }
    return lock_and_iterator.transaction().CheckAborted();
  }

  void FillPriorities(
      boost::container::small_vector_base<std::pair<TransactionId, uint64_t>>* inout) {
    // TODO(dtxn) optimize locking
    for (auto& pair : *inout) {
      auto lock_and_iterator = LockAndFind(
          pair.first, "fill priorities"s, TransactionLoadFlags{TransactionLoadFlag::kMustExist});
      if (!lock_and_iterator.found() || lock_and_iterator.transaction().WasAborted()) {
        pair.second = 0; // Minimal priority for already aborted transactions
      } else {
        pair.second = lock_and_iterator.transaction().metadata().priority;
      }
    }
  }

  void Handle(std::unique_ptr<tablet::UpdateTxnOperationState> state, int64_t term) {
    auto txn_status = state->request()->status();
    if (txn_status == TransactionStatus::APPLYING) {
      HandleApplying(std::move(state), term);
      return;
    }

    if (txn_status == TransactionStatus::IMMEDIATE_CLEANUP ||
        txn_status == TransactionStatus::GRACEFUL_CLEANUP) {
      auto cleanup_type = txn_status == TransactionStatus::IMMEDIATE_CLEANUP
          ? CleanupType::kImmediate
          : CleanupType::kGraceful;
      HandleCleanup(std::move(state), term, cleanup_type);
      return;
    }

    auto error_status = STATUS_FORMAT(
        InvalidArgument, "Unexpected status in transaction participant Handle: $0", *state);
    LOG_WITH_PREFIX(DFATAL) << error_status;
    state->CompleteWithStatus(error_status);
  }

  CHECKED_STATUS ProcessReplicated(const ReplicatedData& data) {
    auto id = FullyDecodeTransactionId(data.state.transaction_id());
    if (!id.ok()) {
      return id.status();
    }

    if (data.state.status() == TransactionStatus::APPLYING) {
      return ReplicatedApplying(*id, data);
    } else if (data.state.status() == TransactionStatus::ABORTED) {
      return ReplicatedAborted(*id, data);
    }

    auto status = STATUS_FORMAT(
        InvalidArgument, "Unexpected status in transaction participant ProcessReplicated: $0, $1",
        data.op_id, data.state);
    LOG_WITH_PREFIX(DFATAL) << status;
    return status;
  }

  void Cleanup(TransactionIdSet&& set, TransactionStatusManager* status_manager) {
    auto cleanup_aborts_task = std::make_shared<CleanupAbortsTask>(
        &applier_, std::move(set), &participant_context_, status_manager, LogPrefix());
    cleanup_aborts_task->Prepare(cleanup_aborts_task);
    participant_context_.StrandEnqueue(cleanup_aborts_task.get());
  }

  CHECKED_STATUS ProcessApply(const TransactionApplyData& data) {
    VLOG_WITH_PREFIX(2) << "Apply: " << data.ToString();

    loader_.WaitLoaded(data.transaction_id);

    ScopedRWOperation operation(pending_op_counter_);
    if (!operation.ok()) {
      LOG_WITH_PREFIX(WARNING) << "Process apply rejected";
      return Status::OK();
    }

    bool was_applied = false;

    {
      // It is our last chance to load transaction metadata, if missing.
      // Because it will be deleted when intents are applied.
      // We are not trying to cleanup intents here because we don't know whether this transaction
      // has intents of not.
      auto lock_and_iterator = LockAndFind(
          data.transaction_id, "pre apply"s, TransactionLoadFlags{TransactionLoadFlag::kMustExist});
      if (!lock_and_iterator.found()) {
        // This situation is normal and could be caused by 2 scenarios:
        // 1) Write batch failed, but originator doesn't know that.
        // 2) Failed to notify status tablet that we applied transaction.
        YB_LOG_WITH_PREFIX_EVERY_N_SECS(WARNING, 1)
            << Format("Apply of unknown transaction: $0", data);
        NotifyApplied(data);
        CHECK(!FLAGS_TEST_fail_in_apply_if_no_metadata);
        return Status::OK();
      }

      auto existing_commit_ht = lock_and_iterator.transaction().local_commit_time();
      if (existing_commit_ht) {
        was_applied = true;
        LOG_WITH_PREFIX(INFO) << "Transaction already applied: " << data.transaction_id;
        LOG_IF_WITH_PREFIX(DFATAL, data.commit_ht != existing_commit_ht)
            << "Transaction was previously applied with another commit ht: " << existing_commit_ht
            << ", new commit ht: " << data.commit_ht;
      } else {
        transactions_.modify(lock_and_iterator.iterator, [&data](auto& txn) {
          txn->SetLocalCommitTime(data.commit_ht);
        });

        LOG_IF_WITH_PREFIX(DFATAL, data.log_ht < last_safe_time_)
            << "Apply transaction before last safe time " << data.transaction_id
            << ": " << data.log_ht << " vs " << last_safe_time_;
      }
    }

    if (!was_applied) {
      auto apply_state = CHECK_RESULT(applier_.ApplyIntents(data));

      VLOG_WITH_PREFIX(4) << "TXN: " << data.transaction_id << ": apply state: "
                          << apply_state.ToString();

      UpdateAppliedTransaction(data, apply_state);
    }

    NotifyApplied(data);
    return Status::OK();
  }

  void UpdateAppliedTransaction(
       const TransactionApplyData& data,
       const docdb::ApplyTransactionState& apply_state) NO_THREAD_SAFETY_ANALYSIS {
    MinRunningNotifier min_running_notifier(&applier_);
    // We are not trying to cleanup intents here because we don't know whether this transaction
    // has intents or not.
    auto lock_and_iterator = LockAndFind(
        data.transaction_id, "apply"s, TransactionLoadFlags{TransactionLoadFlag::kMustExist});
    if (lock_and_iterator.found()) {
      if (!apply_state.active()) {
        RemoveUnlocked(lock_and_iterator.iterator, "applied"s, &min_running_notifier);
      } else {
        lock_and_iterator.transaction().SetApplyData(apply_state, &data);
      }
    }
  }

  void NotifyApplied(const TransactionApplyData& data) {
    VLOG_WITH_PREFIX(4) << Format("NotifyApplied($0)", data);

    if (data.leader_term != OpId::kUnknownTerm) {
      tserver::UpdateTransactionRequestPB req;
      req.set_tablet_id(data.status_tablet);
      auto& state = *req.mutable_state();
      state.set_transaction_id(data.transaction_id.data(), data.transaction_id.size());
      state.set_status(TransactionStatus::APPLIED_IN_ONE_OF_INVOLVED_TABLETS);
      state.add_tablets(participant_context_.tablet_id());
      auto client_result = client();
      if (!client_result.ok()) {
        LOG_WITH_PREFIX(WARNING) << "Get client failed: " << client_result.status();
        return;
      }

      auto handle = rpcs_.Prepare();
      if (handle != rpcs_.InvalidHandle()) {
        *handle = UpdateTransaction(
            TransactionRpcDeadline(),
            nullptr /* remote_tablet */,
            *client_result,
            &req,
            [this, handle](const Status& status,
                           const tserver::UpdateTransactionResponsePB& resp) {
              client::UpdateClock(resp, &participant_context_);
              rpcs_.Unregister(handle);
              LOG_IF_WITH_PREFIX(WARNING, !status.ok()) << "Failed to send applied: " << status;
            });
        (**handle).SendRpc();
      }
    }
  }

  CHECKED_STATUS ProcessCleanup(const TransactionApplyData& data, CleanupType cleanup_type) {
    loader_.WaitLoaded(data.transaction_id);

    MinRunningNotifier min_running_notifier(&applier_);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = transactions_.find(data.transaction_id);
    if (it == transactions_.end()) {
      if (cleanup_type == CleanupType::kImmediate) {
        cleanup_cache_.Insert(data.transaction_id);
      }

      return Status::OK();
    }

    if ((**it).ProcessingApply()) {
      VLOG_WITH_PREFIX(2) << "Don't cleanup transaction because it is applying intents: "
                          << data.transaction_id;
      return Status::OK();
    }

    if (cleanup_type == CleanupType::kGraceful) {
      graceful_cleanup_queue_.push_back(GracefulCleanupQueueEntry{
        .request_id = request_serial_,
        .transaction_id = data.transaction_id,
        .required_safe_time = participant_context_.Now(),
      });
      return Status::OK();
    }

    if (!RemoveUnlocked(it, "cleanup"s, &min_running_notifier)) {
      VLOG_WITH_PREFIX(2) << "Have added aborted txn to cleanup queue: "
                          << data.transaction_id;
    }

    return Status::OK();
  }

  void SetDB(
      const docdb::DocDB& db, const docdb::KeyBounds* key_bounds,
      RWOperationCounter* pending_op_counter) {
    bool had_db = db_.intents != nullptr;
    db_ = db;
    key_bounds_ = key_bounds;
    pending_op_counter_ = pending_op_counter;

    // We should only load transactions on the initial call to SetDB (when opening the tablet), not
    // in case of truncate/restore.
    if (!had_db) {
      loader_.Start(pending_op_counter, db_);
      return;
    }

    loader_.WaitAllLoaded();
    MinRunningNotifier min_running_notifier(&applier_);
    std::lock_guard<std::mutex> lock(mutex_);
    transactions_.clear();
    TransactionsModifiedUnlocked(&min_running_notifier);
  }

  void GetStatus(
      const TransactionId& transaction_id,
      size_t required_num_replicated_batches,
      int64_t term,
      tserver::GetTransactionStatusAtParticipantResponsePB* response,
      rpc::RpcContext* context) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = transactions_.find(transaction_id);
    if (it == transactions_.end()) {
      response->set_num_replicated_batches(0);
      response->set_status_hybrid_time(0);
    } else {
      if ((**it).WasAborted()) {
        response->set_aborted(true);
        return;
      }
      response->set_num_replicated_batches((**it).num_replicated_batches());
      response->set_status_hybrid_time((**it).last_batch_data().hybrid_time.ToUint64());
    }
  }

  TransactionParticipantContext* participant_context() const {
    return &participant_context_;
  }

  HybridTime MinRunningHybridTime() {
    auto result = min_running_ht_.load(std::memory_order_acquire);
    if (result == HybridTime::kMax || result == HybridTime::kInvalid) {
      return result;
    }
    auto now = CoarseMonoClock::now();
    auto current_next_check_min_running = next_check_min_running_.load(std::memory_order_relaxed);
    if (now >= current_next_check_min_running) {
      if (next_check_min_running_.compare_exchange_strong(
              current_next_check_min_running,
              now + 1ms * FLAGS_transaction_min_running_check_interval_ms,
              std::memory_order_acq_rel)) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (transactions_.empty()) {
          return HybridTime::kMax;
        }
        auto& first_txn = **transactions_.get<StartTimeTag>().begin();
        VLOG_WITH_PREFIX(1) << "Checking status of long running min txn " << first_txn.id()
                            << ": " << first_txn.WasAborted();
        static const std::string kRequestReason = "min running check"s;
        // Get transaction status
        auto now_ht = participant_context_.Now();
        StatusRequest status_request = {
            .id = &first_txn.id(),
            .read_ht = now_ht,
            .global_limit_ht = now_ht,
            // Could use 0 here, because read_ht == global_limit_ht.
            // So we cannot accept status with time >= read_ht and < global_limit_ht.
            .serial_no = 0,
            .reason = &kRequestReason,
            .flags = TransactionLoadFlags{},
            .callback = [this, id = first_txn.id()](Result<TransactionStatusResult> result) {
              // Aborted status will result in cleanup of intents.
              VLOG_WITH_PREFIX(1) << "Min running status " << id << ": " << result;
            }
        };
        first_txn.RequestStatusAt(status_request, &lock);
      }
    }
    return result;
  }

  void WaitMinRunningHybridTime(HybridTime ht) {
    MinRunningNotifier min_running_notifier(&applier_);
    std::unique_lock<std::mutex> lock(mutex_);
    waiting_for_min_running_ht_ = ht;
    CheckMinRunningHybridTimeSatisfiedUnlocked(&min_running_notifier);
  }

  CHECKED_STATUS ResolveIntents(HybridTime resolve_at, CoarseTimePoint deadline) {
    RETURN_NOT_OK(WaitUntil(participant_context_.clock_ptr().get(), resolve_at, deadline));

    if (FLAGS_max_transactions_in_status_request == 0) {
      return STATUS(
          IllegalState,
          "Cannot resolve intents when FLAGS_max_transactions_in_status_request is zero");
    }

    std::vector<TransactionId> recheck_ids, committed_ids;

    // Maintain a set of transactions, check their statuses, and remove them as they get
    // committed/applied, aborted or we realize that transaction was not committed at
    // resolve_at.
    for (;;) {
      TransactionStatusResolver resolver(
          &participant_context_, &rpcs_, FLAGS_max_transactions_in_status_request,
          [this, resolve_at, &recheck_ids, &committed_ids](
              const std::vector <TransactionStatusInfo>& status_infos) {
            std::vector<TransactionId> aborted;
            for (const auto& info : status_infos) {
              VLOG_WITH_PREFIX(4) << "Transaction status: " << info.ToString();
              if (info.status == TransactionStatus::COMMITTED) {
                if (info.status_ht <= resolve_at) {
                  // Transaction was committed, but not yet applied.
                  // So rely on filtering recheck_ids before next phase.
                  committed_ids.push_back(info.transaction_id);
                }
              } else if (info.status == TransactionStatus::ABORTED) {
                aborted.push_back(info.transaction_id);
              } else {
                LOG_IF_WITH_PREFIX(DFATAL, info.status != TransactionStatus::PENDING)
                    << "Transaction is in unexpected state: " << info.ToString();
                if (info.status_ht <= resolve_at) {
                  recheck_ids.push_back(info.transaction_id);
                }
              }
            }
            if (!aborted.empty()) {
              MinRunningNotifier min_running_notifier(&applier_);
              std::lock_guard<std::mutex> lock(mutex_);
              for (const auto& id : aborted) {
                EnqueueRemoveUnlocked(id, &min_running_notifier);
              }
            }
          });
      auto se = ScopeExit([&resolver] {
        resolver.Shutdown();
      });
      {
        std::lock_guard <std::mutex> lock(mutex_);
        if (recheck_ids.empty() && committed_ids.empty()) {
          // First step, check all transactions.
          for (const auto& transaction : transactions_) {
            if (!transaction->local_commit_time().is_valid()) {
              resolver.Add(transaction->metadata().status_tablet, transaction->id());
            }
          }
        } else {
          for (const auto& id : recheck_ids) {
            auto it = transactions_.find(id);
            if (it == transactions_.end() || (**it).local_commit_time().is_valid()) {
              continue;
            }
            resolver.Add((**it).metadata().status_tablet, id);
          }
          auto filter = [this](const TransactionId& id) {
            auto it = transactions_.find(id);
            return it == transactions_.end() || (**it).local_commit_time().is_valid();
          };
          committed_ids.erase(std::remove_if(committed_ids.begin(), committed_ids.end(), filter),
                              committed_ids.end());
        }
      }

      recheck_ids.clear();
      resolver.Start(deadline);

      RETURN_NOT_OK(resolver.ResultFuture().get());

      if (recheck_ids.empty()) {
        if (committed_ids.empty()) {
          break;
        } else {
          // We are waiting only for committed transactions to be applied.
          // So just add some delay.
          std::this_thread::sleep_for(10ms * std::min<size_t>(10, committed_ids.size()));
        }
      }
    }

    return Status::OK();
  }

  size_t TEST_GetNumRunningTransactions() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto txn_to_id = [](const RunningTransactionPtr& txn) {
      return txn->id();
    };
    VLOG_WITH_PREFIX(4) << "Transactions: " << AsString(transactions_, txn_to_id)
                        << ", requests: " << AsString(running_requests_);
    return transactions_.size();
  }

  OneWayBitmap TEST_TransactionReplicatedBatches(const TransactionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = transactions_.find(id);
    return it != transactions_.end() ? (**it).replicated_batches() : OneWayBitmap();
  }

  std::string DumpTransactions() {
    std::string result;
    std::lock_guard<std::mutex> lock(mutex_);

    result += Format(
        "{ safe_time_for_participant: $0 remove_queue_size: $1 ",
        participant_context_.SafeTimeForTransactionParticipant(), remove_queue_.size());
    if (!remove_queue_.empty()) {
      result += "remove_queue_front: " + AsString(remove_queue_.front());
    }
    if (!running_requests_.empty()) {
      result += "running_requests_front: " + AsString(running_requests_.front());
    }
    result += "}\n";

    for (const auto& txn : transactions_.get<StartTimeTag>()) {
      result += txn->ToString();
      result += "\n";
    }
    return result;
  }

  CHECKED_STATUS StopActiveTxnsPriorTo(HybridTime cutoff, CoarseTimePoint deadline) {
    vector<TransactionId> ids_to_abort;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto& txn : transactions_.get<StartTimeTag>()) {
        if (txn->start_ht() > cutoff) {
          break;
        }
        if (!txn->WasAborted()) {
          ids_to_abort.push_back(txn->id());
        }
      }
    }

    if (ids_to_abort.empty()) {
      return Status::OK();
    }

    // It is ok to attempt to abort txns that have committed. We don't care
    // if our request succeeds or not.
    CountDownLatch latch(ids_to_abort.size());
    std::atomic<bool> failed{false};
    Status return_status = Status::OK();
    for (const auto& id : ids_to_abort) {
      Abort(
          id, [this, id, &failed, &return_status, &latch](Result<TransactionStatusResult> result) {
            VLOG_WITH_PREFIX(2) << "Aborting " << id << " got " << result;
            if (!result ||
                (result->status != TransactionStatus::COMMITTED && result->status != ABORTED)) {
              LOG(INFO) << "Could not abort " << id << " got " << result;

              bool expected = false;
              if (failed.compare_exchange_strong(expected, true)) {
                if (!result) {
                  return_status = result.status();
                } else {
                  return_status =
                      STATUS_FORMAT(IllegalState, "Wrong status after abort: $0", result->status);
                }
              }
            }
            latch.CountDown();
          });
    }

    return latch.WaitUntil(deadline) ? return_status
                                     : STATUS(TimedOut, "TimedOut while aborting old transactions");
  }

 private:
  class AbortCheckTimeTag;
  class StartTimeTag;

  typedef boost::multi_index_container<RunningTransactionPtr,
      boost::multi_index::indexed_by <
          boost::multi_index::hashed_unique <
              boost::multi_index::const_mem_fun <
                  RunningTransaction, const TransactionId&, &RunningTransaction::id>
          >,
          boost::multi_index::ordered_non_unique <
              boost::multi_index::tag<StartTimeTag>,
              boost::multi_index::const_mem_fun <
                  RunningTransaction, HybridTime, &RunningTransaction::start_ht>
          >,
          boost::multi_index::ordered_non_unique <
              boost::multi_index::tag<AbortCheckTimeTag>,
              boost::multi_index::const_mem_fun <
                  RunningTransaction, HybridTime, &RunningTransaction::abort_check_ht>
          >
      >
  > Transactions;

  void CompleteLoad(const std::function<void()>& functor) override {
    MinRunningNotifier min_running_notifier(&applier_);
    std::lock_guard<std::mutex> lock(mutex_);
    functor();
    TransactionsModifiedUnlocked(&min_running_notifier);
  }

  void LoadFinished(const ApplyStatesMap& pending_applies) override {
    start_latch_.Wait();
    if (closing_.load(std::memory_order_acquire)) {
      LOG_WITH_PREFIX(INFO) << __func__ << ": closing, not starting transaction status resolution";
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto& p : pending_applies) {
        auto it = transactions_.find(p.first);
        if (it == transactions_.end()) {
          LOG_WITH_PREFIX(INFO) << "Unknown transaction for pending apply: " << AsString(p.first);
          continue;
        }

        TransactionApplyData apply_data;
        apply_data.transaction_id = p.first;
        apply_data.commit_ht = p.second.commit_ht;
        (**it).SetApplyData(p.second.state, &apply_data);
      }
    }

    {
      LOG_WITH_PREFIX(INFO) << __func__ << ": starting transaction status resolution";
      std::lock_guard<std::mutex> lock(status_resolvers_mutex_);
      for (auto& status_resolver : status_resolvers_) {
        status_resolver.Start(CoarseTimePoint::max());
      }
    }

    poller_.Start(
        &participant_context_.scheduler(), 1ms * FLAGS_transactions_status_poll_interval_ms);
  }

  void TransactionsModifiedUnlocked(MinRunningNotifier* min_running_notifier) REQUIRES(mutex_) {
    metric_transactions_running_->set_value(transactions_.size());
    if (!loader_.complete()) {
      return;
    }

    if (transactions_.empty()) {
      min_running_ht_.store(HybridTime::kMax, std::memory_order_release);
      CheckMinRunningHybridTimeSatisfiedUnlocked(min_running_notifier);
      return;
    }

    auto& first_txn = **transactions_.get<StartTimeTag>().begin();
    if (first_txn.start_ht() != min_running_ht_.load(std::memory_order_relaxed)) {
      min_running_ht_.store(first_txn.start_ht(), std::memory_order_release);
      next_check_min_running_.store(
          CoarseMonoClock::now() + 1ms * FLAGS_transaction_min_running_check_delay_ms,
          std::memory_order_release);
      CheckMinRunningHybridTimeSatisfiedUnlocked(min_running_notifier);
      return;
    }
  }

  void EnqueueRemoveUnlocked(
      const TransactionId& id, MinRunningNotifier* min_running_notifier) REQUIRES(mutex_) override {
    auto now = participant_context_.Now();
    VLOG_WITH_PREFIX(4) << "EnqueueRemoveUnlocked: " << id << " at " << now;
    remove_queue_.emplace_back(RemoveQueueEntry{id, now});
    ProcessRemoveQueueUnlocked(min_running_notifier);
  }

  void ProcessRemoveQueueUnlocked(MinRunningNotifier* min_running_notifier) REQUIRES(mutex_) {
    if (!remove_queue_.empty()) {
      // When a transaction participant receives an "aborted" response from the coordinator,
      // it puts this transaction into a "remove queue", also storing the current hybrid
      // time. Then queue entries where time is less than current safe time are removed.
      //
      // This is correct because, from a transaction participant's point of view:
      //
      // (1) After we receive a response for a transaction status request, and
      // learn that the transaction is unknown to the coordinator, our local
      // hybrid time is at least as high as the local hybrid time on the
      // transaction status coordinator at the time the transaction was deleted
      // from the coordinator, due to hybrid time propagation on RPC response.
      //
      // (2) If our safe time is greater than the hybrid time when the
      // transaction was deleted from the coordinator, then we have already
      // applied this transaction's provisional records if the transaction was
      // committed.
      auto safe_time = participant_context_.SafeTimeForTransactionParticipant();
      if (!safe_time.is_valid()) {
        VLOG_WITH_PREFIX(3) << "Unable to obtain safe time to check remove queue";
        return;
      }
      VLOG_WITH_PREFIX(3) << "Checking remove queue: " << safe_time << ", "
                          << remove_queue_.front().time << ", " << remove_queue_.front().id;
      LOG_IF_WITH_PREFIX(DFATAL, safe_time < last_safe_time_)
          << "Safe time decreased: " << safe_time << " vs " << last_safe_time_;
      last_safe_time_ = safe_time;
      while (!remove_queue_.empty()) {
        auto it = transactions_.find(remove_queue_.front().id);
        if (it == transactions_.end() || (**it).local_commit_time().is_valid()) {
          // It is regular case, since the coordinator returns ABORTED for already applied
          // transaction. But this particular tablet could not yet apply it, so
          // it would add such transaction to remove queue.
          // And it is the main reason why we are waiting for safe time, before removing intents.
          VLOG_WITH_PREFIX(4) << "Evicting txn from remove queue, w/o removing intents: "
                              << remove_queue_.front().id;
          remove_queue_.pop_front();
          continue;
        }
        if (safe_time <= remove_queue_.front().time) {
          break;
        }
        VLOG_WITH_PREFIX(4) << "Removing from remove queue: " << remove_queue_.front().id;
        static const std::string kRemoveFromQueue = "remove_queue"s;
        RemoveUnlocked(remove_queue_.front().id, kRemoveFromQueue, min_running_notifier);
        remove_queue_.pop_front();
      }
    }
  }

  // Tries to remove transaction with specified id.
  // Returns true if transaction is not exists after call to this method, otherwise returns false.
  // Which means that transaction will be removed later.
  bool RemoveUnlocked(
      const TransactionId& id, const std::string& reason,
      MinRunningNotifier* min_running_notifier) REQUIRES(mutex_) override {
    auto it = transactions_.find(id);
    if (it == transactions_.end()) {
      return true;
    }
    return RemoveUnlocked(it, reason, min_running_notifier);
  }

  bool RemoveUnlocked(
      const Transactions::iterator& it, const std::string& reason,
      MinRunningNotifier* min_running_notifier) REQUIRES(mutex_) {
    if (running_requests_.empty()) {
      (**it).ScheduleRemoveIntents(*it);
      TransactionId txn_id = (**it).id();
      RemoveTransaction(it, min_running_notifier);
      VLOG_WITH_PREFIX(2) << "Cleaned transaction: " << txn_id << ", reason: " << reason
                          << ", left: " << transactions_.size();
      return true;
    }

    // We cannot remove the transaction at this point, because there are running requests
    // that are reading the provisional DB and could request status of this transaction.
    // So we store transaction in a queue and wait when all requests that we launched before our
    // attempt to remove this transaction are completed.
    // Since we try to remove the transaction after all its records are removed from the provisional
    // DB, it is safe to complete removal at this point, because it means that there will be no more
    // queries to status of this transactions.
    immediate_cleanup_queue_.push_back(ImmediateCleanupQueueEntry{
      .request_id = request_serial_,
      .transaction_id = (**it).id(),
    });
    VLOG_WITH_PREFIX(2) << "Queued for cleanup: " << (**it).id() << ", reason: " << reason;
    return false;
  }

  struct LockAndFindResult {
    static Transactions::const_iterator UninitializedIterator() {
      static const Transactions empty_transactions;
      return empty_transactions.end();
    }

    std::unique_lock<std::mutex> lock;
    Transactions::const_iterator iterator = UninitializedIterator();
    bool recently_removed = false;

    bool found() const {
      return lock.owns_lock();
    }

    RunningTransaction& transaction() const {
      return **iterator;
    }
  };

  LockAndFindResult LockAndFind(
      const TransactionId& id, const std::string& reason, TransactionLoadFlags flags) {
    loader_.WaitLoaded(id);
    bool recently_removed;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      auto it = transactions_.find(id);
      if (it != transactions_.end()) {
        return LockAndFindResult{ std::move(lock), it };
      }
      recently_removed = WasTransactionRecentlyRemoved(id);
    }
    if (recently_removed) {
      VLOG_WITH_PREFIX(1)
          << "Attempt to load recently removed transaction: " << id << ", for: " << reason;
      LockAndFindResult result;
      result.recently_removed = true;
      return result;
    }
    metric_transaction_not_found_->Increment();
    if (flags.Test(TransactionLoadFlag::kMustExist)) {
      YB_LOG_WITH_PREFIX_EVERY_N_SECS(WARNING, 1)
          << "Transaction not found: " << id << ", for: " << reason;
    } else {
      YB_LOG_WITH_PREFIX_EVERY_N_SECS(INFO, 1)
          << "Transaction not found: " << id << ", for: " << reason;
    }
    if (flags.Test(TransactionLoadFlag::kCleanup)) {
      VLOG_WITH_PREFIX(2) << "Schedule cleanup for: " << id;
      auto cleanup_task = std::make_shared<CleanupIntentsTask>(
          &participant_context_, &applier_, id);
      cleanup_task->Prepare(cleanup_task);
      participant_context_.StrandEnqueue(cleanup_task.get());
    }
    return LockAndFindResult{};
  }

  void LoadTransaction(
      TransactionMetadata&& metadata,
      TransactionalBatchData&& last_batch_data,
      OneWayBitmap&& replicated_batches,
      const ApplyStateWithCommitHt* pending_apply) override {
    MinRunningNotifier min_running_notifier(&applier_);
    std::lock_guard<std::mutex> lock(mutex_);
    auto txn = std::make_shared<RunningTransaction>(
        std::move(metadata), std::move(last_batch_data), std::move(replicated_batches),
        participant_context_.Now().AddDelta(1ms * FLAGS_transaction_abort_check_timeout_ms), this);
    if (pending_apply) {
      VLOG_WITH_PREFIX(4) << "Apply state found for " << txn->id() << ": "
                          << pending_apply->ToString();
      txn->SetLocalCommitTime(pending_apply->commit_ht);
      txn->SetApplyData(pending_apply->state);
    }
    transactions_.insert(txn);
    TransactionsModifiedUnlocked(&min_running_notifier);
  }

  Result<client::YBClient*> client() const {
    auto cached_value = client_cache_.load(std::memory_order_acquire);
    if (cached_value != nullptr) {
      return cached_value;
    }
    auto future_status = participant_context_.client_future().wait_for(
        TransactionRpcTimeout().ToSteadyDuration());
    if (future_status != std::future_status::ready) {
      return STATUS(TimedOut, "Client not ready");
    }
    auto result = participant_context_.client_future().get();
    client_cache_.store(result, std::memory_order_release);
    return result;
  }

  const std::string& LogPrefix() const override {
    return log_prefix_;
  }

  void RemoveTransaction(Transactions::iterator it, MinRunningNotifier* min_running_notifier)
      REQUIRES(mutex_) {
    auto now = CoarseMonoClock::now();
    CleanupRecentlyRemovedTransactions(now);
    auto& transaction = **it;
    recently_removed_transactions_cleanup_queue_.push_back({transaction.id(), now + 15s});
    LOG_IF_WITH_PREFIX(DFATAL, !recently_removed_transactions_.insert(transaction.id()).second)
        << "Transaction removed twice: " << transaction.id();
    VLOG_WITH_PREFIX(4) << "Remove transaction: " << transaction.id();
    transactions_.erase(it);
    TransactionsModifiedUnlocked(min_running_notifier);
  }

  void CleanupRecentlyRemovedTransactions(CoarseTimePoint now) {
    while (!recently_removed_transactions_cleanup_queue_.empty() &&
           recently_removed_transactions_cleanup_queue_.front().time <= now) {
      recently_removed_transactions_.erase(recently_removed_transactions_cleanup_queue_.front().id);
      recently_removed_transactions_cleanup_queue_.pop_front();
    }
  }

  bool WasTransactionRecentlyRemoved(const TransactionId& id) {
    CleanupRecentlyRemovedTransactions(CoarseMonoClock::now());
    return recently_removed_transactions_.count(id) != 0;
  }

  void CheckMinRunningHybridTimeSatisfiedUnlocked(
      MinRunningNotifier* min_running_notifier) {
    if (min_running_ht_.load(std::memory_order_acquire) <= waiting_for_min_running_ht_) {
      return;
    }
    waiting_for_min_running_ht_ = HybridTime::kMax;
    min_running_notifier->Satisfied();
  }

  void TransactionsStatus(const std::vector<TransactionStatusInfo>& status_infos) {
    MinRunningNotifier min_running_notifier(&applier_);
    std::lock_guard<std::mutex> lock(mutex_);
    HybridTime now = participant_context_.Now();
    for (const auto& info : status_infos) {
      auto it = transactions_.find(info.transaction_id);
      if (it == transactions_.end()) {
        continue;
      }
      if ((**it).UpdateStatus(info.status, info.status_ht)) {
        EnqueueRemoveUnlocked(info.transaction_id, &min_running_notifier);
      } else {
        transactions_.modify(it, [now](const auto& txn) {
          txn->UpdateAbortCheckHT(now, UpdateAbortCheckHTMode::kStatusResponseReceived);
        });
      }
    }
  }

  void HandleApplying(std::unique_ptr<tablet::UpdateTxnOperationState> state, int64_t term) {
    if (RandomActWithProbability(GetAtomicFlag(
        &FLAGS_TEST_transaction_ignore_applying_probability_in_tests))) {
      VLOG_WITH_PREFIX(2)
          << "TEST: Rejected apply: "
          << FullyDecodeTransactionId(state->request()->transaction_id());
      state->CompleteWithStatus(Status::OK());
      return;
    }
    participant_context_.SubmitUpdateTransaction(std::move(state), term);
  }

  void HandleCleanup(
      std::unique_ptr<tablet::UpdateTxnOperationState> state, int64_t term,
      CleanupType cleanup_type) {
    VLOG_WITH_PREFIX(3) << "Cleanup";
    auto id = FullyDecodeTransactionId(state->request()->transaction_id());
    if (!id.ok()) {
      state->CompleteWithStatus(id.status());
      return;
    }

    TransactionApplyData data = {
        .leader_term = term,
        .transaction_id = *id,
        .op_id = OpIdPB(),
        .commit_ht = HybridTime(),
        .log_ht = HybridTime(),
        .sealed = state->request()->sealed(),
        .status_tablet = std::string()
    };
    WARN_NOT_OK(ProcessCleanup(data, cleanup_type), "Process cleanup failed");
    state->CompleteWithStatus(Status::OK());
  }

  CHECKED_STATUS ReplicatedApplying(const TransactionId& id, const ReplicatedData& data) {
    // data.state.tablets contains only status tablet.
    if (data.state.tablets_size() != 1) {
      return STATUS_FORMAT(InvalidArgument,
                           "Expected only one table during APPLYING, state received: $0",
                           data.state);
    }
    HybridTime commit_time(data.state.commit_hybrid_time());
    TransactionApplyData apply_data = {
        data.leader_term, id, data.op_id, commit_time, data.hybrid_time, data.sealed,
        data.state.tablets(0) };
    if (!data.already_applied_to_regular_db) {
      return ProcessApply(apply_data);
    }
    if (!data.sealed) {
      return ProcessCleanup(apply_data, CleanupType::kImmediate);
    }
    return Status::OK();
  }

  CHECKED_STATUS ReplicatedAborted(const TransactionId& id, const ReplicatedData& data) {
    MinRunningNotifier min_running_notifier(&applier_);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = transactions_.find(id);
    if (it == transactions_.end()) {
      TransactionMetadata metadata = {
        .transaction_id = id,
        .isolation = IsolationLevel::NON_TRANSACTIONAL,
        .status_tablet = TabletId(),
        .priority = 0
      };
      it = transactions_.insert(std::make_shared<RunningTransaction>(
          metadata, TransactionalBatchData(), OneWayBitmap(), HybridTime::kMax, this)).first;
      TransactionsModifiedUnlocked(&min_running_notifier);
    }

    // TODO(dtxn) store this fact to rocksdb.
    (**it).Aborted();

    return Status::OK();
  }

  void Poll() {
    {
      MinRunningNotifier min_running_notifier(&applier_);
      std::lock_guard<std::mutex> lock(mutex_);

      ProcessRemoveQueueUnlocked(&min_running_notifier);
      if (ANNOTATE_UNPROTECTED_READ(FLAGS_transactions_poll_check_aborted)) {
        CheckForAbortedTransactions();
      }
    }
    CleanupStatusResolvers();
  }

  void CheckForAbortedTransactions() REQUIRES(mutex_) {
    if (transactions_.empty()) {
      return;
    }
    auto now = participant_context_.Now();
    auto& index = transactions_.get<AbortCheckTimeTag>();
    TransactionStatusResolver* resolver = nullptr;
    for (;;) {
      auto& txn = **index.begin();
      if (txn.abort_check_ht() > now) {
        break;
      }
      if (!resolver) {
        resolver = &AddStatusResolver();
      }
      const auto& metadata = txn.metadata();
      VLOG_WITH_PREFIX(4)
          << "Check aborted: " << metadata.status_tablet << ", " << metadata.transaction_id;
      resolver->Add(metadata.status_tablet, metadata.transaction_id);
      index.modify(index.begin(), [now](const auto& txn) {
        txn->UpdateAbortCheckHT(now, UpdateAbortCheckHTMode::kStatusRequestSent);
      });
    }

    // We don't introduce limit on number of status resolutions here, because we cannot predict
    // transactions throughput. And we rely the logic that we cannot start multiple resolutions
    // for single transaction because we set abort check hybrid time to the same value as
    // status resolution deadline.
    if (resolver) {
      resolver->Start(CoarseMonoClock::now() + 1ms * FLAGS_transaction_abort_check_timeout_ms);
    }
  }

  void CleanupStatusResolvers() EXCLUDES(status_resolvers_mutex_) {
    std::lock_guard<std::mutex> lock(status_resolvers_mutex_);
    while (!status_resolvers_.empty() && !status_resolvers_.front().Running()) {
      status_resolvers_.front().Shutdown();
      status_resolvers_.pop_front();
    }
  }

  TransactionStatusResolver& AddStatusResolver() override EXCLUDES(status_resolvers_mutex_) {
    std::lock_guard<std::mutex> lock(status_resolvers_mutex_);
    status_resolvers_.emplace_back(&
        participant_context_, &rpcs_, FLAGS_max_transactions_in_status_request,
        std::bind(&Impl::TransactionsStatus, this, _1));
    return status_resolvers_.back();
  }

  struct ImmediateCleanupQueueEntry {
    int64_t request_id;
    TransactionId transaction_id;

    bool Ready(TransactionParticipantContext* participant_context, HybridTime* safe_time) const {
      return true;
    }
  };

  struct GracefulCleanupQueueEntry {
    int64_t request_id;
    TransactionId transaction_id;
    HybridTime required_safe_time;

    bool Ready(TransactionParticipantContext* participant_context, HybridTime* safe_time) const {
      if (!*safe_time) {
        *safe_time = participant_context->SafeTimeForTransactionParticipant();
      }
      return *safe_time >= required_safe_time;
    }
  };

  std::string log_prefix_;

  docdb::DocDB db_;
  const docdb::KeyBounds* key_bounds_;
  // Owned externally, should be guaranteed that would not be destroyed before this.
  RWOperationCounter* pending_op_counter_ = nullptr;

  Transactions transactions_;
  // Ids of running requests, stored in increasing order.
  std::deque<int64_t> running_requests_;
  // Ids of complete requests, minimal request is on top.
  // Contains only ids greater than first running request id, otherwise entry is removed
  // from both collections.
  std::priority_queue<int64_t, std::vector<int64_t>, std::greater<void>> complete_requests_;

  // Queues of transaction ids that should be cleaned, paired with request that should be completed
  // in order to be able to do clean.
  // Immediate cleanup is performed as soon as possible.
  // Graceful cleanup is performed after safe time becomes greater than cleanup request hybrid time.
  std::deque<ImmediateCleanupQueueEntry> immediate_cleanup_queue_ GUARDED_BY(mutex_);
  std::deque<GracefulCleanupQueueEntry> graceful_cleanup_queue_ GUARDED_BY(mutex_);

  // Remove queue maintains transactions that could be cleaned when safe time for follower reaches
  // appropriate time for an entry.
  // Since we add entries with increasing time, this queue is ordered by time.
  struct RemoveQueueEntry {
    TransactionId id;
    HybridTime time;

    std::string ToString() const {
      return Format("{ id: $0 time: $1 }", id, time);
    }
  };

  // Guarded by RunningTransactionContext::mutex_
  std::deque<RemoveQueueEntry> remove_queue_;

  // Guarded by RunningTransactionContext::mutex_
  HybridTime last_safe_time_ = HybridTime::kMin;

  std::unordered_set<TransactionId, TransactionIdHash> recently_removed_transactions_;
  struct RecentlyRemovedTransaction {
    TransactionId id;
    CoarseTimePoint time;
  };
  std::deque<RecentlyRemovedTransaction> recently_removed_transactions_cleanup_queue_;

  std::mutex status_resolvers_mutex_;
  std::deque<TransactionStatusResolver> status_resolvers_ GUARDED_BY(status_resolvers_mutex_);

  scoped_refptr<AtomicGauge<uint64_t>> metric_transactions_running_;
  scoped_refptr<Counter> metric_transaction_not_found_;

  TransactionLoader loader_;
  std::atomic<bool> closing_{false};
  CountDownLatch start_latch_{1};

  std::atomic<HybridTime> min_running_ht_{HybridTime::kInvalid};
  std::atomic<CoarseTimePoint> next_check_min_running_{CoarseTimePoint()};
  HybridTime waiting_for_min_running_ht_ = HybridTime::kMax;
  std::atomic<bool> shutdown_done_{false};

  mutable std::atomic<client::YBClient*> client_cache_{nullptr};

  LRUCache<TransactionId> cleanup_cache_{FLAGS_transactions_cleanup_cache_size};

  rpc::Poller poller_;
};

TransactionParticipant::TransactionParticipant(
    TransactionParticipantContext* context, TransactionIntentApplier* applier,
    const scoped_refptr<MetricEntity>& entity)
    : impl_(new Impl(context, applier, entity)) {
}

TransactionParticipant::~TransactionParticipant() {
}

void TransactionParticipant::Start() {
  impl_->Start();
}

bool TransactionParticipant::Add(
    const TransactionMetadataPB& data, rocksdb::WriteBatch *write_batch) {
  return impl_->Add(data, write_batch);
}

Result<TransactionMetadata> TransactionParticipant::PrepareMetadata(
    const TransactionMetadataPB& pb) {
  return impl_->PrepareMetadata(pb);
}

boost::optional<std::pair<IsolationLevel, TransactionalBatchData>>
    TransactionParticipant::PrepareBatchData(
    const TransactionId& id, size_t batch_idx,
    boost::container::small_vector_base<uint8_t>* encoded_replicated_batches) {
  return impl_->PrepareBatchData(id, batch_idx, encoded_replicated_batches);
}

void TransactionParticipant::BatchReplicated(
    const TransactionId& id, const TransactionalBatchData& data) {
  return impl_->BatchReplicated(id, data);
}

HybridTime TransactionParticipant::LocalCommitTime(const TransactionId& id) {
  return impl_->LocalCommitTime(id);
}

std::pair<size_t, size_t> TransactionParticipant::TEST_CountIntents() const {
  return impl_->TEST_CountIntents();
}

void TransactionParticipant::RequestStatusAt(const StatusRequest& request) {
  return impl_->RequestStatusAt(request);
}

int64_t TransactionParticipant::RegisterRequest() {
  return impl_->RegisterRequest();
}

void TransactionParticipant::UnregisterRequest(int64_t request) {
  impl_->UnregisterRequest(request);
}

void TransactionParticipant::Abort(const TransactionId& id,
                                   TransactionStatusCallback callback) {
  return impl_->Abort(id, std::move(callback));
}

void TransactionParticipant::Handle(
    std::unique_ptr<tablet::UpdateTxnOperationState> request, int64_t term) {
  impl_->Handle(std::move(request), term);
}

void TransactionParticipant::Cleanup(TransactionIdSet&& set) {
  return impl_->Cleanup(std::move(set), this);
}

Status TransactionParticipant::ProcessReplicated(const ReplicatedData& data) {
  return impl_->ProcessReplicated(data);
}

Status TransactionParticipant::CheckAborted(const TransactionId& id) {
  return impl_->CheckAborted(id);
}

void TransactionParticipant::FillPriorities(
    boost::container::small_vector_base<std::pair<TransactionId, uint64_t>>* inout) {
  return impl_->FillPriorities(inout);
}

void TransactionParticipant::SetDB(
    const docdb::DocDB& db, const docdb::KeyBounds* key_bounds,
    RWOperationCounter* pending_op_counter) {
  impl_->SetDB(db, key_bounds, pending_op_counter);
}

void TransactionParticipant::GetStatus(
    const TransactionId& transaction_id,
    size_t required_num_replicated_batches,
    int64_t term,
    tserver::GetTransactionStatusAtParticipantResponsePB* response,
    rpc::RpcContext* context) {
  impl_->GetStatus(transaction_id, required_num_replicated_batches, term, response, context);
}

TransactionParticipantContext* TransactionParticipant::context() const {
  return impl_->participant_context();
}

HybridTime TransactionParticipant::MinRunningHybridTime() const {
  return impl_->MinRunningHybridTime();
}

void TransactionParticipant::WaitMinRunningHybridTime(HybridTime ht) {
  impl_->WaitMinRunningHybridTime(ht);
}

Status TransactionParticipant::ResolveIntents(HybridTime resolve_at, CoarseTimePoint deadline) {
  return impl_->ResolveIntents(resolve_at, deadline);
}

size_t TransactionParticipant::TEST_GetNumRunningTransactions() const {
  return impl_->TEST_GetNumRunningTransactions();
}

OneWayBitmap TransactionParticipant::TEST_TransactionReplicatedBatches(
    const TransactionId& id) const {
  return impl_->TEST_TransactionReplicatedBatches(id);
}

std::string TransactionParticipant::ReplicatedData::ToString() const {
  return YB_STRUCT_TO_STRING(leader_term, state, op_id, hybrid_time, already_applied_to_regular_db);
}

void TransactionParticipant::StartShutdown() {
  impl_->StartShutdown();
}

void TransactionParticipant::CompleteShutdown() {
  impl_->CompleteShutdown();
}

std::string TransactionParticipant::DumpTransactions() const {
  return impl_->DumpTransactions();
}

Status TransactionParticipant::StopActiveTxnsPriorTo(HybridTime cutoff, CoarseTimePoint deadline) {
  return impl_->StopActiveTxnsPriorTo(cutoff, deadline);
}

std::string TransactionParticipantContext::LogPrefix() const {
  return consensus::MakeTabletLogPrefix(tablet_id(), permanent_uuid());
}

HybridTime TransactionParticipantContext::Now() {
  return clock_ptr()->Now();
}

} // namespace tablet
} // namespace yb
