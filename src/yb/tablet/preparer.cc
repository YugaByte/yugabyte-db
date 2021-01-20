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

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <gflags/gflags.h>

#include "yb/consensus/consensus.h"
#include "yb/gutil/macros.h"
#include "yb/tablet/preparer.h"
#include "yb/tablet/operations/operation_driver.h"

#include "yb/util/flag_tags.h"
#include "yb/util/logging.h"
#include "yb/util/threadpool.h"
#include "yb/util/lockfree.h"

DEFINE_int32(max_group_replicate_batch_size, 16,
             "Maximum number of operations to submit to consensus for replication in a batch.");

DEFINE_test_flag(int32, preparer_batch_inject_latency_ms, 0,
                 "Inject latency before replicating batch.");

using namespace std::literals;
using std::vector;

namespace yb {
class ThreadPool;
class ThreadPoolToken;

namespace tablet {

// ------------------------------------------------------------------------------------------------
// PreparerImpl

class PreparerImpl {
 public:
  explicit PreparerImpl(consensus::Consensus* consensus, ThreadPool* tablet_prepare_pool);
  ~PreparerImpl();
  CHECKED_STATUS Start();
  void Stop();

  CHECKED_STATUS Submit(OperationDriver* operation_driver);

  ThreadPoolToken* PoolToken() {
    return tablet_prepare_pool_token_.get();
  }

 private:
  using OperationDrivers = std::vector<OperationDriver*>;

  consensus::Consensus* const consensus_;

  // We set this to true to tell the Run function to return. No new tasks will be accepted, but
  // existing tasks will still be processed.
  std::atomic<bool> stop_requested_{false};

  // If true, a task is running for this tablet already.
  // If false, no tasks are running for this tablet,
  // and we can submit a task to the thread pool token.
  std::atomic<bool> running_{false};

  // This is set to true immediately before the thread exits.
  std::atomic<bool> stopped_{false};

  // Number or active tasks is incremented before task added to queue and decremented after
  // it was popped.
  // So it always greater than or equal to number of entries in queue.
  std::atomic<int64_t> active_tasks_{0};

  // This flag is used in a sanity check to ensure that after this server becomes a follower,
  // the earlier leader-side operations that are still in the preparer's queue should fail to get
  // prepared due to old term. This sanity check will only be performed when an UpdateConsensus
  // with follower-side operations is received while earlier leader-side operations still have not
  // been processed, e.g. in an overloaded tablet server with lots of leader changes.
  std::atomic<bool> prepare_should_fail_{false};

  MPSCQueue<OperationDriver> queue_;

  // This mutex/condition combination is used in Stop() in case multiple threads are calling that
  // function concurrently. One of them will ask the prepare thread to stop and wait for it, and
  // then will notify other threads that have called Stop().
  std::mutex stop_mtx_;
  std::condition_variable stop_cond_;

  OperationDrivers leader_side_batch_;

  std::unique_ptr<ThreadPoolToken> tablet_prepare_pool_token_;

  // A temporary buffer of rounds to replicate, used to reduce reallocation.
  consensus::ConsensusRounds rounds_to_replicate_;

  void Run();
  void ProcessItem(OperationDriver* item);

  void ProcessAndClearLeaderSideBatch();

  // A wrapper around ProcessAndClearLeaderSideBatch that assumes we are currently holding the
  // mutex.

  void ReplicateSubBatch(OperationDrivers::iterator begin,
                         OperationDrivers::iterator end);
};

PreparerImpl::PreparerImpl(consensus::Consensus* consensus, ThreadPool* tablet_prepare_pool)
    : consensus_(consensus),
      tablet_prepare_pool_token_(tablet_prepare_pool
                                     ->NewToken(ThreadPool::ExecutionMode::SERIAL)) {
}

PreparerImpl::~PreparerImpl() {
  Stop();
}

Status PreparerImpl::Start() {
  return Status::OK();
}

void PreparerImpl::Stop() {
  if (stopped_.load(std::memory_order_acquire)) {
    return;
  }
  stop_requested_ = true;
  {
    std::unique_lock<std::mutex> stop_lock(stop_mtx_);
    stop_cond_.wait(stop_lock, [this] {
      return !running_.load(std::memory_order_acquire) &&
             active_tasks_.load(std::memory_order_acquire) == 0;
    });
  }
  stopped_.store(true, std::memory_order_release);
}

Status PreparerImpl::Submit(OperationDriver* operation_driver) {
  if (stop_requested_.load(std::memory_order_acquire)) {
    return STATUS(IllegalState, "Tablet is shutting down");
  }

  const bool leader_side = operation_driver->is_leader_side();

  // When a leader becomes a follower, we expect the leader-side operations still in the preparer's
  // queue to fail to be prepared because their term will be too old as we try to add them to the
  // Raft queue.
  prepare_should_fail_.store(!leader_side, std::memory_order_release);

  if (leader_side) {
    // Prepare leader-side operations on the "preparer thread" so we can only acquire the
    // ReplicaState lock once and append multiple operations.
    active_tasks_.fetch_add(1, std::memory_order_release);
    queue_.Push(operation_driver);
  } else {
    // For follower-side operations, there would be no benefit in preparing them on the preparer
    // thread.
    operation_driver->PrepareAndStartTask();
  }

  auto expected = false;
  if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    // running_ was already true, so we are not creating a task to process operations.
    return Status::OK();
  }
  // We flipped running_ from 0 to 1. The previously running thread could go back to doing another
  // iteration, but in that case since we are submitting to a token of a thread pool, only one
  // such thread will be running, the other will be in the queue.
  return tablet_prepare_pool_token_->SubmitFunc(std::bind(&PreparerImpl::Run, this));
}

void PreparerImpl::Run() {
  VLOG(2) << "Starting prepare task:" << this;
  for (;;) {
    while (OperationDriver *item = queue_.Pop()) {
      active_tasks_.fetch_sub(1, std::memory_order_release);
      ProcessItem(item);
    }
    ProcessAndClearLeaderSideBatch();
    std::unique_lock<std::mutex> stop_lock(stop_mtx_);
    running_.store(false, std::memory_order_release);
    // Check whether tasks were added while we were setting running to false.
    if (active_tasks_.load(std::memory_order_acquire)) {
      // Got more operations, try stay in the loop.
      bool expected = false;
      if (running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        continue;
      }
      // If someone else has flipped running_ to true, we can safely exit this function because
      // another task is already submitted to the same token.
    }
    if (stop_requested_.load(std::memory_order_acquire)) {
      VLOG(2) << "Prepare task's Run() function is returning because stop is requested.";
      stop_cond_.notify_all();
    }
    VLOG(2) << "Returning from prepare task after inactivity: " << this;
    return;
  }
}

namespace {

bool ShouldApplySeparately(OperationType operation_type) {
  switch (operation_type) {
    // For certain operations types we have to apply them in a batch of their own.
    // E.g. ChangeMetadataOperation::Prepare calls Tablet::CreatePreparedChangeMetadata, which
    // acquires the schema lock. Because of this, we must not attempt to process two
    // ChangeMetadataOperations in one batch, otherwise we'll deadlock.
    //
    // Also, for infrequently occuring operations batching has little performance benefit in
    // general.
    case OperationType::kChangeMetadata: FALLTHROUGH_INTENDED;
    case OperationType::kSnapshot: FALLTHROUGH_INTENDED;
    case OperationType::kTruncate: FALLTHROUGH_INTENDED;
    case OperationType::kSplit: FALLTHROUGH_INTENDED;
    case OperationType::kEmpty: FALLTHROUGH_INTENDED;
    case OperationType::kHistoryCutoff:
      return true;

    case OperationType::kWrite: FALLTHROUGH_INTENDED;
    case OperationType::kUpdateTransaction:
      return false;
  }
  FATAL_INVALID_ENUM_VALUE(OperationType, operation_type);
}

}  // anonymous namespace

void PreparerImpl::ProcessItem(OperationDriver* item) {
  CHECK_NOTNULL(item);

  LOG_IF(DFATAL, !item->is_leader_side()) << "Processing follower-side item";

  auto operation_type = item->operation_type();

  const bool apply_separately = ShouldApplySeparately(operation_type);
  const int64_t bound_term = apply_separately ? -1 : item->consensus_round()->bound_term();

  // Don't add more than the max number of operations to a batch, and also don't add
  // operations bound to different terms, so as not to fail unrelated operations
  // unnecessarily in case of a bound term mismatch.
  if (leader_side_batch_.size() >= FLAGS_max_group_replicate_batch_size ||
      (!leader_side_batch_.empty() &&
          bound_term != leader_side_batch_.back()->consensus_round()->bound_term())) {
    ProcessAndClearLeaderSideBatch();
  }
  leader_side_batch_.push_back(item);
  if (apply_separately) {
    ProcessAndClearLeaderSideBatch();
  }
}

void PreparerImpl::ProcessAndClearLeaderSideBatch() {
  if (leader_side_batch_.empty()) {
    return;
  }

  VLOG(2) << "Preparing a batch of " << leader_side_batch_.size() << " leader-side operations";

  auto iter = leader_side_batch_.begin();
  auto replication_subbatch_begin = iter;
  auto replication_subbatch_end = iter;

  // PrepareAndStart does not call Consensus::Replicate anymore as of 07/07/2017, and it is our
  // responsibility to do so in case of success. We call Consensus::ReplicateBatch for batches
  // of consecutive successfully prepared operations.

  while (iter != leader_side_batch_.end()) {
    auto* operation_driver = *iter;

    Status s = operation_driver->PrepareAndStart();

    if (PREDICT_TRUE(s.ok())) {
      replication_subbatch_end = ++iter;
    } else {
      ReplicateSubBatch(replication_subbatch_begin, replication_subbatch_end);

      // Handle failure for this operation itself.
      operation_driver->HandleFailure(s);

      // Now we'll start accumulating a new batch.
      replication_subbatch_begin = replication_subbatch_end = ++iter;
    }
  }

  // Replicate the remaining batch. No-op for an empty batch.
  ReplicateSubBatch(replication_subbatch_begin, replication_subbatch_end);

  leader_side_batch_.clear();
}

void PreparerImpl::ReplicateSubBatch(
    OperationDrivers::iterator batch_begin,
    OperationDrivers::iterator batch_end) {
  DCHECK_GE(std::distance(batch_begin, batch_end), 0);
  if (batch_begin == batch_end) {
    return;
  }
  VLOG(2) << "Replicating a sub-batch of " << std::distance(batch_begin, batch_end)
          << " leader-side operations";
  if (VLOG_IS_ON(3)) {
    for (auto batch_iter = batch_begin; batch_iter != batch_end; ++batch_iter) {
      VLOG(3) << "Leader-side operation to be replicated: " << (*batch_iter)->ToString();
    }
  }

  rounds_to_replicate_.clear();
  rounds_to_replicate_.reserve(std::distance(batch_begin, batch_end));
  for (auto batch_iter = batch_begin; batch_iter != batch_end; ++batch_iter) {
    DCHECK_ONLY_NOTNULL(*batch_iter);
    DCHECK_ONLY_NOTNULL((*batch_iter)->consensus_round());
    rounds_to_replicate_.push_back((*batch_iter)->consensus_round());
  }

  AtomicFlagSleepMs(&FLAGS_TEST_preparer_batch_inject_latency_ms);
  // Have to save this value before calling replicate batch.
  // Because the following scenario is legal:
  // Operation successfully processed by ReplicateBatch, but ReplicateBatch did not return yet.
  // Submit of follower side operation is called from another thread.
  bool should_fail = prepare_should_fail_.load(std::memory_order_acquire);
  const Status s = consensus_->ReplicateBatch(&rounds_to_replicate_);
  rounds_to_replicate_.clear();

  if (PREDICT_FALSE(!s.ok())) {
    VLOG(1) << "ReplicateBatch failed with status " << s.ToString()
            << ", treating all " << std::distance(batch_begin, batch_end) << " operations as "
            << "failed with that status";
    // Treat all the operations in the batch as failed.
    for (auto batch_iter = batch_begin; batch_iter != batch_end; ++batch_iter) {
      (*batch_iter)->ReplicationFailed(s);
    }
  } else if (should_fail) {
    LOG(DFATAL) << "Operations should fail, but was successfully prepared: "
                << AsString(boost::make_iterator_range(batch_begin, batch_end));
  }
}

// ------------------------------------------------------------------------------------------------
// Preparer

Preparer::Preparer(consensus::Consensus* consensus, ThreadPool* tablet_prepare_thread)
    : impl_(std::make_unique<PreparerImpl>(consensus, tablet_prepare_thread)) {
}

Preparer::~Preparer() = default;

Status Preparer::Start() {
  VLOG(1) << "Starting the preparer";
  return impl_->Start();
}

void Preparer::Stop() {
  VLOG(1) << "Stopping the preparer";
  impl_->Stop();
  VLOG(1) << "The preparer has stopped";
}

Status Preparer::Submit(OperationDriver* operation_driver) {
  return impl_->Submit(operation_driver);
}

ThreadPoolToken* Preparer::PoolToken() {
  return impl_->PoolToken();
}

}  // namespace tablet
}  // namespace yb
