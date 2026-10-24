// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugabyteDB development.
//
// Portions Copyright (c) YugabyteDB, Inc.
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

#include "yb/tablet/tablet_peer.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "yb/client/client.h"

#include "yb/common/transaction_error.h"

#include "yb/consensus/consensus.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/consensus_util.h"
#include "yb/consensus/log.h"
#include "yb/consensus/log_anchor_registry.h"
#include "yb/consensus/raft_consensus.h"
#include "yb/consensus/retryable_requests.h"
#include "yb/consensus/state_change_context.h"

#include "yb/docdb/consensus_frontier.h"

#include "yb/fs/fs_manager.h"

#include "yb/gutil/casts.h"
#include "yb/gutil/strings/join.h"

#include "yb/master/master_ddl.pb.h"

#include "yb/rocksdb/db/memtable.h"

#include "yb/rpc/messenger.h"
#include "yb/rpc/periodic.h"

#include "yb/tablet/operations/change_auto_flags_config_operation.h"
#include "yb/tablet/operations/change_metadata_operation.h"
#include "yb/tablet/operations/clone_operation.h"
#include "yb/tablet/operations/history_cutoff_operation.h"
#include "yb/tablet/operations/operation_driver.h"
#include "yb/tablet/operations/snapshot_operation.h"
#include "yb/tablet/operations/split_operation.h"
#include "yb/tablet/operations/truncate_operation.h"
#include "yb/tablet/operations/update_txn_operation.h"
#include "yb/tablet/operations/write_operation.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet.pb.h"
#include "yb/tablet/tablet_bootstrap_if.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/tablet_metrics.h"
#include "yb/tablet/tablet_peer_mm_ops.h"
#include "yb/tablet/tablet_retention_policy.h"
#include "yb/tablet/tablet_vector_indexes.h"
#include "yb/tablet/transaction_participant.h"
#include "yb/tablet/write_query.h"

#include "yb/tserver/tserver_error.h"

#include "yb/util/fault_injection.h"
#include "yb/util/format.h"
#include "yb/util/logging.h"
#include "yb/util/metrics.h"
#include "yb/util/status_format.h"
#include "yb/util/status_log.h"
#include "yb/util/stopwatch.h"
#include "yb/util/sync_point.h"
#include "yb/util/threadpool.h"
#include "yb/util/trace.h"

using namespace std::literals;
using namespace std::placeholders;

using std::shared_ptr;
using std::string;
using std::vector;

DEFINE_test_flag(int32, delay_init_tablet_peer_ms, 0,
                 "Wait before executing init tablet peer for specified amount of milliseconds.");

DEFINE_test_flag(bool, tablet_peer_force_get_raft_consensus_failure, false,
                 "If set, TabletPeer::GetRaftConsensus returns IllegalState immediately. "
                 "Used to deterministically exercise error paths in callers that handle "
                 "transient consensus unavailability.");

DEFINE_UNKNOWN_int32(cdc_min_replicated_index_considered_stale_secs, 1800,
    "If cdc_min_replicated_index hasn't been replicated in this amount of time, we reset its"
    "value to max int64 to avoid retaining any logs");

DEFINE_RUNTIME_int32(cdc_min_replicated_index_considered_stale_secs_master, 14400 /* 4 hours */,
    "Same as cdc_min_replicated_index_considered_stale_secs but applied to the sys catalog tablet "
    "on master. A higher default (4 hours) prevents premature reset of retention barriers on "
    "master followers that receive less frequent barrier updates.");

DEFINE_UNKNOWN_bool(propagate_safe_time, true,
    "Propagate safe time to read from leader to followers");

DEFINE_RUNTIME_bool(abort_active_txns_during_xrepl_bootstrap, true,
    "Abort active transactions during xcluster and cdc bootstrapping. Inconsistent replicated data "
    "may be produced if this is disabled.");
TAG_FLAG(abort_active_txns_during_xrepl_bootstrap, advanced);

DEFINE_RUNTIME_uint32(log_retention_diagnostics_min_age_secs, 0,
    "When the maintenance manager computes the GC-able WAL size, log WAL retention diagnostics if "
    "the first retained segment is at least this old in seconds. 0 disables.");
TAG_FLAG(log_retention_diagnostics_min_age_secs, advanced);

DECLARE_int32(ysql_transaction_abort_timeout_ms);

DECLARE_bool(cdc_immediate_transaction_cleanup);

DECLARE_uint64(cdc_intent_retention_ms);

DECLARE_bool(enable_flush_retryable_requests);

namespace yb::tablet {

METRIC_DEFINE_event_stats(table, op_prepare_queue_length, "Operation Prepare Queue Length",
                        MetricUnit::kTasks,
                        "Number of operations waiting to be prepared within this tablet. "
                        "High queue lengths indicate that the server is unable to process "
                        "operations as fast as they are being written to the WAL.");

METRIC_DEFINE_event_stats(table, op_prepare_queue_time,
    "Operation Prepare Queue Time", MetricUnit::kMicroseconds,
    "Time (microseconds) that operations spent waiting in the prepare queue before being "
    "processed. High queue times indicate that the server is unable to "
    "process operations as fast as they are being written to the WAL.");

METRIC_DEFINE_event_stats(table, op_prepare_run_time, "Operation Prepare Run Time",
                        MetricUnit::kMicroseconds,
                        "Time (microseconds) that operations spent being prepared in the tablet. "
                        "High values may indicate that the server is under-provisioned or "
                        "that operations are experiencing high contention with one another for "
                        "locks.");

using consensus::Consensus;
using consensus::ConsensusBootstrapInfo;
using consensus::ConsensusMetadata;
using consensus::ConsensusOptions;
using consensus::ConsensusRound;
using consensus::PeerMemberType;
using consensus::RaftConfigPB;
using consensus::RaftConsensus;
using consensus::RaftPeerPB;
using consensus::StateChangeContext;
using consensus::StateChangeReason;
using log::Log;
using log::LogAnchorRegistry;
using rpc::Messenger;
using tserver::TabletServerErrorPB;

// ============================================================================
//  Tablet Peer
// ============================================================================
TabletPeer::TabletPeer(
    const RaftGroupMetadataPtr& meta,
    const consensus::RaftPeerPB& local_peer_pb,
    const scoped_refptr<server::Clock>& clock,
    const std::string& permanent_uuid,
    Callback<void(std::shared_ptr<StateChangeContext> context)> mark_dirty_clbk,
    MetricRegistry* metric_registry,
    TabletSplitter* tablet_splitter,
    const std::shared_future<client::YBClient*>& client_future)
    : meta_(meta),
      tablet_id_(meta->raft_group_id()),
      local_peer_pb_(local_peer_pb),
      state_(RaftGroupStatePB::NOT_STARTED),
      operation_tracker_(consensus::MakeTabletLogPrefix(tablet_id_, permanent_uuid)),
      status_listener_(new TabletStatusListener(meta)),
      clock_(clock),
      log_anchor_registry_(new LogAnchorRegistry()),
      mark_dirty_clbk_(std::move(mark_dirty_clbk)),
      permanent_uuid_(permanent_uuid),
      preparing_operations_counter_(operation_tracker_.LogPrefix()),
      metric_registry_(metric_registry),
      tablet_splitter_(tablet_splitter),
      client_future_(client_future),
      data_disk_space_checker_(meta->fs_manager()->env(), meta->data_root_dir()) {}

TabletPeer::~TabletPeer() {
  std::lock_guard lock(lock_);
  // We should either have called Shutdown(), or we should have never called
  // Init().
  LOG_IF_WITH_PREFIX(DFATAL, tablet_) << "TabletPeer not fully shut down.";
}

void TabletPeer::SetPerDbCgroup(Cgroup* cgroup) {
  {
    std::lock_guard l(lock_);
    if (consensus_) {
      consensus_->SetPerDbCgroup(cgroup);
    }
  }
  if (log_) {
    log_->SetPerDbCgroup(cgroup);
  }
  if (prepare_thread_) {
    prepare_thread_->SetPerDbCgroup(cgroup);
  }
}

Status TabletPeer::InitTabletPeer(
    const TabletPtr& tablet,
    const std::shared_ptr<MemTracker>& server_mem_tracker,
    Messenger* messenger,
    rpc::ThreadPoolPtr service_pool,
    rpc::ProxyCache* proxy_cache,
    const scoped_refptr<Log>& log,
    const scoped_refptr<MetricEntity>& table_metric_entity,
    const scoped_refptr<MetricEntity>& tablet_metric_entity,
    ThreadPool* raft_pool,
    ThreadPool* snapshot_cleanup_pool,
    rpc::ThreadPool* raft_notifications_pool,
    ThreadPool* tablet_prepare_pool,
    consensus::RetryableRequests* retryable_requests,
    std::shared_ptr<TabletBootstrapStateManager> bootstrap_state_manager,
    std::unique_ptr<ConsensusMetadata> consensus_meta,
    consensus::MultiRaftManager* multi_raft_manager,
    ThreadPool* flush_bootstrap_state_pool) {
  DCHECK(tablet) << "A TabletPeer must be provided with a Tablet";
  DCHECK(log) << "A TabletPeer must be provided with a Log";

  if (FLAGS_TEST_delay_init_tablet_peer_ms > 0) {
    std::this_thread::sleep_for(FLAGS_TEST_delay_init_tablet_peer_ms * 1ms);
  }

  // Additional `consensus` variable is required for the out-of-lock-block usage in this method.
  shared_ptr<consensus::RaftConsensus> consensus;
  {
    std::lock_guard lock(lock_);
    auto state = state_.load(std::memory_order_acquire);
    if (state != RaftGroupStatePB::BOOTSTRAPPING) {
      return STATUS_FORMAT(
          IllegalState, "Invalid tablet state for init: $0", RaftGroupStatePB_Name(state));
    }
    tablet_ = tablet;
    tablet_weak_ = tablet;
    log_ = log;
    // "Publish" the log pointer so it can be retrieved using the log() accessor.
    log_atomic_ = log.get();

    service_thread_pool_holder_ = std::move(service_pool);
    service_thread_pool_.store(service_thread_pool_holder_.get(), std::memory_order_release);
    strand_.reset(new rpc::Strand(service_thread_pool_holder_.get()));
    messenger_ = messenger;

    tablet->SetMemTableFlushFilterFactory([log] {
      auto largest_log_op_index = log->GetLatestEntryOpId().index;
      return [largest_log_op_index] (const rocksdb::MemTable& memtable, bool) -> Result<bool> {
        auto frontiers = memtable.Frontiers();
        if (frontiers) {
          const auto largest_memtable_op_index =
              down_cast<const docdb::ConsensusFrontier&>(frontiers->Largest()).op_id().index;
          // We can only flush this memtable if all operations written to it have also been written
          // to the log (maybe not synced, if durable_wal_write is disabled, but that's OK).
          auto should_flush = largest_memtable_op_index <= largest_log_op_index;
          if (!should_flush) {
            LOG(WARNING)
              << "Skipping flush on memtable with ops ahead of log. "
              << "Memtable index: " << largest_memtable_op_index
              << " - log index: " << largest_log_op_index;
          }
          return should_flush;
        }

        // It is correct to not have frontiers when memtable is empty
        if (memtable.IsEmpty()) {
          return true;
        }

        // This is a degenerate case that should ideally never occur. An empty memtable got into the
        // list of immutable memtables. We say it is OK to flush it and move on.
        static const char* error_msg =
            "A memtable with no frontiers set found when deciding what memtables to "
            "flush! This should not happen.";
        LOG(DFATAL) << error_msg;
        return STATUS(IllegalState, error_msg);
      };
    });

    tablet_->SetCleanupPool(snapshot_cleanup_pool, &messenger_->scheduler(), raft_pool);

    ConsensusOptions options;
    options.tablet_id = meta_->raft_group_id();

    TRACE("Creating consensus instance");

    if (!consensus_meta) {
      RETURN_NOT_OK(ConsensusMetadata::Load(meta_->fs_manager(), tablet_id_,
                                            meta_->fs_manager()->uuid(), &consensus_meta));
    }

    consensus_ = RaftConsensus::Create(
        options,
        std::move(consensus_meta),
        local_peer_pb_,
        table_metric_entity,
        tablet_metric_entity,
        clock_,
        this,
        messenger,
        proxy_cache,
        log_.get(),
        server_mem_tracker,
        tablet_->mem_tracker(),
        mark_dirty_clbk_,
        tablet_->table_type(),
        raft_pool,
        raft_notifications_pool,
        retryable_requests,
        multi_raft_manager);
    has_consensus_.store(true, std::memory_order_release);
    consensus = consensus_;

    bootstrap_state_manager_ = bootstrap_state_manager;

    auto flush_bootstrap_state_pool_token = flush_bootstrap_state_pool
        ? flush_bootstrap_state_pool->NewToken(ThreadPool::ExecutionMode::SERIAL) : nullptr;
    bootstrap_state_flusher_ = std::make_shared<TabletBootstrapStateFlusher>(
        tablet_id_, tablet_weak_, consensus_, bootstrap_state_manager_,
        std::move(flush_bootstrap_state_pool_token));

    tablet_->SetHybridTimeLeaseProvider(std::bind(&TabletPeer::HybridTimeLease, this, _1, _2));
    operation_tracker_.SetPostTracker(
        std::bind(&RaftConsensus::TrackOperationMemory, consensus_.get(), _1));

    prepare_thread_ = std::make_unique<Preparer>(consensus_.get(), tablet_prepare_pool);

    auto txn_participant = tablet_->transaction_participant();
    if (txn_participant) {
      txn_participant->SetMinReplayTxnFirstWriteTimeUpdateCallback(
          std::bind_front(&TabletPeer::MinReplayTxnFirstWriteTimeUpdated, this));
    }

    // "Publish" the tablet object right before releasing the lock.
    tablet_obj_state_.store(TabletObjectState::kAvailable, std::memory_order_release);
  }
  // End of lock scope for lock_.

  auto raft_config = consensus->CommittedConfig();
  ChangeConfigReplicated(raft_config);  // Set initial flag value.

  RETURN_NOT_OK(prepare_thread_->Start());

  if (tablet_->metrics() != nullptr) {
    TRACE("Starting instrumentation");
    operation_tracker_.StartInstrumentation(tablet_->GetTabletMetricsEntity());
  }
  operation_tracker_.StartMemoryTracking(tablet_->mem_tracker());

  RETURN_NOT_OK(set_cdc_min_replicated_index(meta_->cdc_min_replicated_index()));
  RETURN_NOT_OK(set_cdc_sdk_safe_time(meta_->cdc_sdk_safe_time()));

  TRACE("TabletPeer::Init() finished");
  VLOG_WITH_PREFIX(2) << "Peer Initted";

  return Status::OK();
}

Result<FixedHybridTimeLease> TabletPeer::HybridTimeLease(
    HybridTime min_allowed, CoarseTimePoint deadline) {
  HybridTime time;
  {
    SCOPED_WAIT_STATUS(WaitForReadTime);
    time = VERIFY_RESULT(WaitUntil(clock_.get(), min_allowed, deadline));
  }
  // min_allowed could contain non zero logical part, so we add one microsecond to be sure that
  // the resulting ht_lease is at least min_allowed.
  auto min_allowed_micros = min_allowed.CeilPhysicalValueMicros();
  MicrosTime lease_micros =
      VERIFY_RESULT(VERIFY_RESULT(GetConsensus())
                        ->MajorityReplicatedHtLeaseExpiration(min_allowed_micros, deadline));
  if (lease_micros >= kMaxHybridTimePhysicalMicros) {
    // This could happen when leader leases are disabled.
    return FixedHybridTimeLease();
  }
  return FixedHybridTimeLease {
    .time = time,
    .lease = HybridTime(lease_micros, /* logical */ 0)
  };
}

Result<HybridTime> TabletPeer::PreparePeerRequest() {
  auto leader_term =
      VERIFY_RESULT(GetRaftConsensus())->GetLeaderState(/* allow_stale= */ true).term;
  if (leader_term >= 0) {
    auto last_write_ht = tablet_->mvcc_manager()->LastReplicatedHybridTime();
    auto propagated_history_cutoff =
        tablet_->RetentionPolicy()->HistoryCutoffToPropagate(last_write_ht);

    if (propagated_history_cutoff.cotables_cutoff_ht ||
        propagated_history_cutoff.primary_cutoff_ht) {
      VLOG_WITH_PREFIX(2) << "Propagate history cutoff: " << propagated_history_cutoff;

      auto operation = std::make_unique<HistoryCutoffOperation>(tablet_);
      auto request = operation->AllocateRequest();
      if (propagated_history_cutoff.primary_cutoff_ht) {
        request->set_primary_cutoff_ht(
            propagated_history_cutoff.primary_cutoff_ht.ToUint64());
      }
      if (propagated_history_cutoff.cotables_cutoff_ht) {
        request->set_cotables_cutoff_ht(
            propagated_history_cutoff.cotables_cutoff_ht.ToUint64());
      }
      Submit(std::move(operation), leader_term);
    }
  }

  if (!FLAGS_propagate_safe_time) {
    return HybridTime::kInvalid;
  }

  // Get the current majority-replicated HT leader lease without any waiting.
  auto ht_lease = VERIFY_RESULT(HybridTimeLease(
      /* min_allowed= */ HybridTime::kMin, /* deadline */ CoarseTimePoint::max()));
  auto safe_time = tablet_->mvcc_manager()->SafeTime(ht_lease);
  return safe_time.ok() ? *safe_time : HybridTime::kInvalid;
}

Status TabletPeer::MajorityReplicated(const OpId& committed_op_id) {
  auto ht_lease = VERIFY_RESULT(HybridTimeLease(
      /* min_allowed= */ HybridTime::kMin, /* deadline */ CoarseTimePoint::max()));

  tablet_->mvcc_manager()->UpdatePropagatedSafeTimeOnLeader(ht_lease);

  NotifyCommitedAsyncWrites(committed_op_id);
  return Status::OK();
}

void TabletPeer::BecomeReplica() {
  // For graceful stepdowns, this gets called after we wait for the protege to catch up (which also
  // waits for any in-progress async writes to make it to the protege). Note that if that drain
  // takes longer than FLAGS_protege_synchronization_timeout_ms, then ongoing async writes will
  // likely abort - FailAllAsyncWrites will make the client retry on the new leader to validate.

  // Return NOT_THE_LEADER so the client can retry on a different leader.
  FailAllAsyncWrites(STATUS(
      IllegalState, Format("Tablet $0 leader changed during async write", tablet_id()),
      tserver::TabletServerError(tserver::TabletServerErrorPB::NOT_THE_LEADER)));
}

void TabletPeer::ChangeConfigReplicated(const RaftConfigPB& config) {
  tablet_->mvcc_manager()->SetLeaderOnlyMode(config.peers_size() == 1);
}

uint64_t TabletPeer::NumSSTFiles() {
  return tablet_->GetCurrentVersionNumSSTFiles();
}

void TabletPeer::ListenNumSSTFilesChanged(std::function<void()> listener) {
  tablet_->ListenNumSSTFilesChanged(std::move(listener));
}

Status TabletPeer::CheckOperationAllowed(const OpId& op_id, consensus::OperationType op_type) {
  return tablet_->CheckOperationAllowed(op_id, op_type);
}

namespace {

// Rejects a write whose WritePB::ignore_after_hybrid_time is not after the hybrid time the op was
// assigned, letting a client with a time-bounded lease stop its writes landing once the lease is
// gone.
Status CheckWriteFence(const consensus::LWReplicateMsg& msg) {
  if (msg.op_type() != consensus::OperationType::WRITE_OP) {
    return Status::OK();
  }
  const auto fence = HybridTime::FromPB(msg.write().ignore_after_hybrid_time());
  if (!fence) {
    return Status::OK();
  }
  // The op's own hybrid time, assigned by NotifyAddedToLeader, is when the write takes effect.
  // An earlier clock reading would leave an unbounded window in which the op could still be
  // assigned a time past the fence.
  const auto hybrid_time = HybridTime(msg.hybrid_time());
  if (fence > hybrid_time) {
    return Status::OK();
  }
  return STATUS_EC_FORMAT(
      Expired, tserver::TabletServerError(TabletServerErrorPB::WRITE_FENCE_EXPIRED),
      "Write is fenced: ignore_after_hybrid_time $0 is not after $1", fence, hybrid_time);
}

}  // namespace

Status TabletPeer::CheckReplicateAllowed(const consensus::LWReplicateMsg& replicate_msg) {
  return CheckWriteFence(replicate_msg);
}

Status TabletPeer::Start(const ConsensusBootstrapInfo& bootstrap_info) {
  {
    std::lock_guard l(state_change_lock_);
    TRACE("Starting consensus");

    VLOG_WITH_PREFIX(2) << "Peer starting";

    auto consensus = GetRaftConsensusUnsafe();
    CHECK_NOTNULL(consensus.get()); // Sanity check, consensus must be alive at this point.
    VLOG(2) << "RaftConfig before starting: " << consensus->CommittedConfig().DebugString();

    // If tablet was previously considered shutdown w.r.t. metrics,
    // fix that for a tablet now being reinstated.
    DVLOG_WITH_PREFIX(3)
      << "Remove from set of tablets that have been shutdown so as to allow reporting metrics";
    metric_registry_->tablets_shutdown_erase(tablet_id());

    RETURN_NOT_OK(consensus->Start(bootstrap_info));
    RETURN_NOT_OK(UpdateState(RaftGroupStatePB::BOOTSTRAPPING, RaftGroupStatePB::RUNNING,
                              "Incorrect state to start TabletPeer, "));
  }
  tablet_->Start();

  // The context tracks that the current caller does not hold the lock for consensus state.
  // So mark dirty callback, e.g., consensus->ConsensusState() for master consensus callback of
  // SysCatalogStateChanged, can get the lock when needed.
  auto context =
      std::make_shared<StateChangeContext>(StateChangeReason::TABLET_PEER_STARTED, false);
  // Because we changed the tablet state, we need to re-report the tablet to the master.
  mark_dirty_clbk_.Run(context);

  return tablet_->CompleteStartup();
}

bool TabletPeer::StartShutdown(
    const DisableFlushOnShutdown disable_flush_on_shutdown, const AbortOps abort_ops) {
  LOG_WITH_PREFIX(INFO) << "Initiating TabletPeer shutdown";

  auto consensus = GetRaftConsensusUnsafe();
  if (consensus) {
    consensus->StartShutdown();
  }

  TabletPtr tablet;
  {
    std::lock_guard lock(lock_);
    DEBUG_ONLY_TEST_SYNC_POINT("TabletPeer::StartShutdown:1");
    tablet = tablet_;
  }
  // Tablet::StartShutdown runs StartShutdownStorages, which may block waiting for in-flight read/
  // write operations to drain, so it must not run while holding the lock_ spinlock.
  if (tablet) {
    tablet->StartShutdown(disable_flush_on_shutdown, abort_ops);
  }

  {
    RaftGroupStatePB state = state_.load(std::memory_order_acquire);
    for (;;) {
      if (state == RaftGroupStatePB::QUIESCING || state == RaftGroupStatePB::SHUTDOWN) {
        return false;
      }
      if (state_.compare_exchange_strong(
          state, RaftGroupStatePB::QUIESCING, std::memory_order_acq_rel)) {
        LOG_WITH_PREFIX(INFO) << "Started shutdown from state: " << RaftGroupStatePB_Name(state);
        break;
      }
    }
  }

  {
    std::lock_guard l(state_change_lock_);
    // Even though Tablet::Shutdown() also unregisters its ops, we have to do it here
    // to ensure that any currently running operation finishes before we proceed with
    // the rest of the shutdown sequence. In particular, a maintenance operation could
    // indirectly end up calling into the log, which we are about to shut down.
    UnregisterMaintenanceOps();

    if (consensus) {
      consensus->CompleteShutdown();
    }
  }

  // Fail parked WaitForAsyncWrite RPCs. During server shutdown, Messenger::Shutdown joins the
  // reactor threads, which waits for all inbound calls to be responded to, and that runs before
  // TabletPeer::CompleteShutdown. Return NOT_THE_LEADER so the client can retry on a new leader.
  FailAllAsyncWrites(STATUS(
      IllegalState, Format("Tablet $0 peer shutting down during async write", tablet_id()),
      tserver::TabletServerError(tserver::TabletServerErrorPB::NOT_THE_LEADER)));

  return true;
}

void TabletPeer::CompleteShutdown() {
  auto* strand = strand_.get();
  if (strand) {
    strand->Shutdown();
  }

  preparing_operations_counter_.Shutdown();

  // TODO: KUDU-183: Keep track of the pending tasks and send an "abort" message.
  LOG_SLOW_EXECUTION(WARNING, 1000,
      Format("TabletPeer: tablet $0: Waiting for Operations to complete", tablet_id())) {
    operation_tracker_.WaitForAllToFinish();
  }

  if (prepare_thread_) {
    prepare_thread_->Stop();
  }

  if (log_) {
    WARN_NOT_OK(log_->Close(), LogPrefix() + "Error closing the Log");
  }

  VLOG_WITH_PREFIX(1) << "Shut down!";

  if (tablet_) {
    tablet_->CompleteShutdown();
  }

  tablet_obj_state_.store(TabletObjectState::kDestroyed, std::memory_order_release);

  // Only mark the peer as SHUTDOWN when all other components have shut down.
  std::shared_ptr<consensus::RaftConsensus> consensus;
  {
    std::lock_guard lock(lock_);
    strand_.reset();
    if (bootstrap_state_flusher_) {
      bootstrap_state_flusher_->Shutdown();
      bootstrap_state_flusher_.reset();
    }
    // Release mem tracker resources.
    has_consensus_.store(false, std::memory_order_release);
    // Clear the consensus and destroy it outside the lock.
    consensus_.swap(consensus);
    prepare_thread_.reset();
    tablet_.reset();
    auto state = state_.load(std::memory_order_acquire);
    LOG_IF_WITH_PREFIX(DFATAL, state != RaftGroupStatePB::QUIESCING) <<
        "Bad state when completing shutdown: " << RaftGroupStatePB_Name(state);
    state_.store(RaftGroupStatePB::SHUTDOWN, std::memory_order_release);

    if (metric_registry_) {
      DVLOG_WITH_PREFIX(3)
        << "Add to set of tablets that have been shutdown so as to avoid reporting metrics";
      metric_registry_->tablets_shutdown_insert(tablet_id());
    }
  }
}

void TabletPeer::WaitUntilShutdown() {
  const MonoDelta kSingleWait = 10ms;
  const MonoDelta kReportInterval = 5s;
  const MonoDelta kMaxWait = 30s;

  MonoDelta waited = MonoDelta::kZero;
  MonoDelta last_reported = MonoDelta::kZero;
  while (state_.load(std::memory_order_acquire) != RaftGroupStatePB::SHUTDOWN) {
    if (waited >= last_reported + kReportInterval) {
      if (waited >= kMaxWait) {
        LOG_WITH_PREFIX(DFATAL)
            << "Wait for shutdown " << waited << " exceeded kMaxWait " << kMaxWait;
      } else {
        LOG_WITH_PREFIX(WARNING) << "Long wait for shutdown: " << waited;
      }
      last_reported = waited;
    }
    SleepFor(kSingleWait);
    waited += kSingleWait;
  }

  if (metric_registry_) {
    DVLOG_WITH_PREFIX(3)
      << "Add to set of tablets that have been shutdown so as to avoid reporting metrics";
    metric_registry_->tablets_shutdown_insert(tablet_id());
  }
}

Status TabletPeer::TEST_Shutdown(
    ShouldAbortActiveTransactions should_abort_active_txns,
    DisableFlushOnShutdown disable_flush_on_shutdown,
    std::optional<TransactionId>&& exclude_aborting_txn_id) {
  auto is_shutdown_initiated =
      StartShutdown(disable_flush_on_shutdown, AbortOps(should_abort_active_txns));

  if (should_abort_active_txns) {
    // Once raft group state enters QUIESCING state,
    // new queries cannot be processed from then onwards.
    // Aborting any remaining active transactions in the tablet.
    AbortActiveTransactions(std::move(exclude_aborting_txn_id));
  }

  if (is_shutdown_initiated) {
    CompleteShutdown();
  } else {
    WaitUntilShutdown();
  }
  return Status::OK();
}

void TabletPeer::AbortActiveTransactions(
    std::optional<TransactionId>&& exclude_aborting_txn_id) const {
  if (!tablet_) {
    return;
  }
  auto deadline =
      CoarseMonoClock::Now() + MonoDelta::FromMilliseconds(FLAGS_ysql_transaction_abort_timeout_ms);
  WARN_NOT_OK(
      tablet_->AbortActiveTransactions(deadline, std::move(exclude_aborting_txn_id)),
      "Cannot abort transactions for tablet " + tablet_->tablet_id());
}

Status TabletPeer::CheckRunning() const {
  auto state = state_.load(std::memory_order_acquire);
  if (state != RaftGroupStatePB::RUNNING) {
    if (state == RaftGroupStatePB::QUIESCING) {
      return STATUS(ShutdownInProgress, "The tablet is shutting down");
    }
    return STATUS_FORMAT(IllegalState, "The tablet is not in a running state: $0",
                         RaftGroupStatePB_Name(state));
  }

  return Status::OK();
}

bool TabletPeer::IsShutdownStarted() const {
  auto state = state_.load(std::memory_order_acquire);
  return state == RaftGroupStatePB::QUIESCING || state == RaftGroupStatePB::SHUTDOWN;
}

Status TabletPeer::CheckShutdownOrNotStarted() const {
  RaftGroupStatePB value = state_.load(std::memory_order_acquire);
  if (value != RaftGroupStatePB::SHUTDOWN && value != RaftGroupStatePB::NOT_STARTED) {
    return STATUS(IllegalState, Format("The tablet is not in a shutdown state: $0",
                                       RaftGroupStatePB_Name(value)));
  }

  return Status::OK();
}

Status TabletPeer::WaitUntilConsensusRunning(const MonoDelta& timeout) {
  MonoTime start(MonoTime::Now());

  int backoff_exp = 0;
  const int kMaxBackoffExp = 8;
  while (true) {
    RaftGroupStatePB cached_state = state_.load(std::memory_order_acquire);
    if (cached_state == RaftGroupStatePB::QUIESCING || cached_state == RaftGroupStatePB::SHUTDOWN) {
      return STATUS(IllegalState,
          Format("The tablet is already shutting down or shutdown. State: $0",
                 RaftGroupStatePB_Name(cached_state)));
    }
    if (cached_state == RUNNING && has_consensus_.load(std::memory_order_acquire) &&
        VERIFY_RESULT(GetRaftConsensus())->IsRunning()) {
      break;
    }
    MonoTime now(MonoTime::Now());
    MonoDelta elapsed(now.GetDeltaSince(start));
    if (elapsed.MoreThan(timeout)) {
      return STATUS(TimedOut, Format("Consensus is not running after waiting for $0. State; $1",
                                     elapsed.ToString(), RaftGroupStatePB_Name(cached_state)));
    }
    SleepFor(MonoDelta::FromMilliseconds(1 << backoff_exp));
    backoff_exp = std::min(backoff_exp + 1, kMaxBackoffExp);
  }
  return Status::OK();
}

void TabletPeer::WriteAsync(std::unique_ptr<WriteQuery> query) {
  ScopedOperation preparing_token(&preparing_operations_counter_);
  auto status = CheckRunning();
  if (!status.ok()) {
    query->Cancel(status);
    return;
  }

  query->operation().set_preparing_token(std::move(preparing_token));
  tablet_->AcquireLocksAndPerformDocOperations(std::move(query));
}

Result<HybridTime> TabletPeer::ReportReadRestart() {
  tablet_->metrics()->Increment(TabletCounters::kRestartReadRequests);
  return tablet_->SafeTime(RequireLease::kTrue);
}

void TabletPeer::Submit(std::unique_ptr<Operation> operation, int64_t term) {
  ScopedOperation preparing_token(&preparing_operations_counter_);
  auto status = CheckRunning();

  if (status.ok()) {
    operation->set_preparing_token(std::move(preparing_token));
    auto driver = NewLeaderOperationDriver(&operation, term);
    if (driver.ok()) {
      (**driver).ExecuteAsync();
    } else {
      status = driver.status();
    }
  }
  if (!status.ok()) {
    operation->Aborted(status, /* was_pending= */ false);
  }
}

Status TabletPeer::SubmitUpdateTransaction(
    std::unique_ptr<UpdateTxnOperation>& operation, int64_t term) {
  if (!operation->tablet_is_set()) {
    auto tablet = VERIFY_RESULT(shared_tablet());
    operation->SetTablet(tablet);
  }
  auto scoped_read_operation = VERIFY_RESULT(operation->tablet_safe())
                                   ->CreateScopedRWOperationBlockingRocksDbShutdownStart();
  if (!scoped_read_operation.ok()) {
    auto status = MoveStatus(scoped_read_operation).CloneAndPrepend(operation->ToString());
    operation->CompleteWithStatus(status);
    return status;
  }
  Submit(std::move(operation), term);
  return Status::OK();
}

HybridTime TabletPeer::SafeTimeForTransactionParticipant() {
  // During tablet bootstrap tablet_ is not yet assigned (InitTabletPeer), and during shutdown it is
  // reset. Vector index backfill runs during bootstrap and can resolve a transaction's status,
  // which walks the remove queue and calls this from a thread that does not hold lock_, so reading
  // the raw tablet_ pointer here can dereference a null tablet. Callers treat an invalid HybridTime
  // as "safe time unavailable".
  auto tablet = shared_tablet_maybe_null();
  if (!tablet) {
    return HybridTime::kInvalid;
  }
  auto safe_time = tablet->mvcc_manager()->SafeTimeForFollower(
      /* min_allowed= */ HybridTime::kMin, /* deadline= */ CoarseTimePoint::min());
  return safe_time.ok() ? *safe_time : HybridTime::kInvalid;
}

Result<HybridTime> TabletPeer::WaitForSafeTime(HybridTime safe_time, CoarseTimePoint deadline) {
  // See SafeTimeForTransactionParticipant: tablet_ may not be assigned yet (bootstrap) or may have
  // been reset (shutdown), and this can be called from vector index backfill without holding lock_.
  auto tablet = VERIFY_RESULT(shared_tablet());
  return tablet->SafeTime(RequireLease::kFallbackToFollower, safe_time, deadline);
}

Status TabletPeer::GetLastReplicatedData(RemoveIntentsData* data) {
  std::shared_ptr<consensus::RaftConsensus> consensus;
  TabletPtr tablet;
  {
    std::lock_guard lock(lock_);
    consensus = consensus_;
    tablet = tablet_;
  }
  if (!consensus) {
    return STATUS(IllegalState, "Consensus destroyed");
  }
  if (!tablet) {
    return STATUS(IllegalState, "Tablet destroyed");
  }
  // TODO(yyan) : we should use the last replicated op id instead of last committed op id.
  data->op_id = consensus->GetLastCommittedOpId();
  data->log_ht = tablet->mvcc_manager()->LastReplicatedHybridTime();
  return Status::OK();
}

void TabletPeer::UpdateClock(HybridTime hybrid_time) {
  clock_->Update(hybrid_time);
}

std::unique_ptr<UpdateTxnOperation> TabletPeer::CreateUpdateTransaction(
    std::shared_ptr<LWTransactionStatePB> request) {
  // TODO: safe handling for the case when tablet is not set.
  auto result = std::make_unique<UpdateTxnOperation>(CHECK_RESULT(shared_tablet()));
  result->TakeRequest(std::move(request));
  return result;
}

void TabletPeer::GetTabletStatusPB(TabletStatusPB* status_pb_out) {
  std::shared_ptr<RaftConsensus> consensus;
  {
    std::lock_guard lock(lock_);
    DCHECK(status_pb_out != nullptr);
    DCHECK(status_listener_.get() != nullptr);
    const auto disk_size_info = GetOnDiskSizeInfoUnlocked();
    status_pb_out->set_tablet_id(status_listener_->tablet_id());
    status_pb_out->set_namespace_name(status_listener_->namespace_name());
    status_pb_out->set_table_name(status_listener_->table_name());
    status_pb_out->set_table_id(status_listener_->table_id());
    status_pb_out->set_last_status(status_listener_->last_status());
    status_listener_->partition()->ToPB(status_pb_out->mutable_partition());
    status_pb_out->set_state(state_);
    status_pb_out->set_tablet_data_state(meta_->tablet_data_state());
    auto tablet = tablet_;
    if (tablet) {
      status_pb_out->set_table_type(tablet->table_type());
      tablet->vector_indexes().FillBackfillStatus(*status_pb_out);
    }
    disk_size_info.ToPB(status_pb_out);
    // Set hide status of the tablet.
    status_pb_out->set_is_hidden(meta_->hidden());
    status_pb_out->set_rocksdb_parent_data_compacted(meta_->rocksdb_parent_data_compacted());
    // Reports whether a compaction is still required, not the physical state: with
    // vector_index_include_into_post_split_compaction off nothing will ever compact
    // the inherited data, and a consumer waiting on this bit would wait forever.
    // Left unset when the tablet is not available, so that consumers don't read
    // an unknown state as compacted.
    if (tablet) {
      status_pb_out->set_vector_indexes_parent_data_compacted(
          !tablet->vector_indexes().PostSplitCompactionRequired());
    }
    for (const auto& table : meta_->GetAllColocatedTables()) {
      status_pb_out->add_colocated_table_ids(table);
    }
    consensus = consensus_;
  }
  if (consensus) {
    consensus->log()->GetLatestEntryOpId().ToPB(status_pb_out->mutable_last_op_id());
  }
}

Status TabletPeer::RunLogGC(bool rollover) {
  if (!CheckRunning().ok()) {
    return Status::OK();
  }
  auto s = reset_cdc_min_replicated_index_if_stale();
  if (!s.ok()) {
    LOG_WITH_PREFIX(WARNING) << "Unable to reset cdc min replicated index " << s;
  }
  log::MinRetainLogIndexInfo min_log_index;
  if (VLOG_IS_ON(2)) {
    std::string details;
    min_log_index = VERIFY_RESULT(GetEarliestNeededLogIndex(&details));
    LOG_WITH_PREFIX(INFO) << __func__ << ": " << details;
  } else {
     min_log_index = VERIFY_RESULT(GetEarliestNeededLogIndex());
  }
  if (rollover) {
    RETURN_NOT_OK(log_->AllocateSegmentAndRollOver());
  }
  int32_t num_gced = 0;
  return log_->GC(min_log_index, &num_gced);
}

Result<TabletPtr> TabletPeer::shared_tablet() const {
  // Note that there is still a possible race condition between the time we check the tablet state
  // and the time we access the tablet shared pointer through the weak pointer.
  auto tablet_obj_state = tablet_obj_state_.load(std::memory_order_acquire);
  if (tablet_obj_state != TabletObjectState::kAvailable) {
    return STATUS_FORMAT(
        IllegalState,
        "Tablet not running: tablet object $0 has invalid state $1",
        tablet_id_, tablet_obj_state);
  }
  // The weak pointer is safe to access as long as the state is not kUninitialized, because it
  // never changes after the state is set to kAvailable. However, we still need to check if
  // lock() returns nullptr.
  auto tablet_ptr = tablet_weak_.lock();
  if (tablet_ptr)
    return tablet_ptr;
  return STATUS_FORMAT(
      IllegalState,
      "Tablet object $0 has already been deallocated",
      tablet_id_);
}

TabletDataState TabletPeer::data_state() const {
  std::lock_guard lock(lock_);
  return meta_->tablet_data_state();
}

string TabletPeer::HumanReadableState() const {
  std::lock_guard lock(lock_);
  TabletDataState data_state = meta_->tablet_data_state();
  RaftGroupStatePB state = this->state();
  // If failed, any number of things could have gone wrong.
  if (state == RaftGroupStatePB::FAILED) {
    return Format("$0 ($1): $2", RaftGroupStatePB_Name(state),
                  TabletDataState_Name(data_state),
                  error_.get()->ToString());
  // If it's remotely bootstrapping, or tombstoned, that is the important thing
  // to show.
  } else if (!CanServeTabletData(data_state)) {
    return TabletDataState_Name(data_state);
  } else if (data_state == TabletDataState::TABLET_DATA_SPLIT_COMPLETED) {
    return RaftGroupStatePB_Name(state) + " (split)";
  }
  // Otherwise, the tablet's data is in a "normal" state, so we just display
  // the runtime state (BOOTSTRAPPING, RUNNING, etc).
  return RaftGroupStatePB_Name(state);
}

namespace {

consensus::OperationType MapOperationTypeToPB(OperationType operation_type) {
  switch (operation_type) {
    case OperationType::kWrite:
      return consensus::WRITE_OP;

    case OperationType::kChangeMetadata:
      return consensus::CHANGE_METADATA_OP;

    case OperationType::kUpdateTransaction:
      return consensus::UPDATE_TRANSACTION_OP;

    case OperationType::kSnapshot:
      return consensus::SNAPSHOT_OP;

    case OperationType::kTruncate:
      return consensus::TRUNCATE_OP;

    case OperationType::kHistoryCutoff:
      return consensus::HISTORY_CUTOFF_OP;

    case OperationType::kSplit:
      return consensus::SPLIT_OP;

    case OperationType::kChangeAutoFlagsConfig:
      return consensus::CHANGE_AUTO_FLAGS_CONFIG_OP;

    case OperationType::kClone:
      return consensus::CLONE_OP;

    case OperationType::kEmpty:
      LOG(FATAL) << "OperationType::kEmpty cannot be converted to consensus::OperationType";
  }
  FATAL_INVALID_ENUM_VALUE(OperationType, operation_type);
}

} // namespace

void TabletPeer::GetInFlightOperations(Operation::TraceType trace_type,
                                       vector<consensus::OperationStatusPB>* out) const {
  for (const auto& driver : operation_tracker_.GetPendingOperations()) {
    if (driver->operation() == nullptr) {
      continue;
    }
    auto op_type = driver->operation_type();
    if (op_type == OperationType::kEmpty) {
      // This is a special-purpose in-memory-only operation for updating propagated safe time on
      // a follower.
      continue;
    }

    consensus::OperationStatusPB status_pb;
    driver->GetOpId().ToPB(status_pb.mutable_op_id());
    status_pb.set_operation_type(MapOperationTypeToPB(op_type));
    status_pb.set_description(driver->ToString());
    int64_t running_for_micros =
        MonoTime::Now().GetDeltaSince(driver->start_time()).ToMicroseconds();
    status_pb.set_running_for_micros(running_for_micros);
    if (trace_type == Operation::TRACE_TXNS && driver->trace()) {
      status_pb.set_trace_buffer(driver->trace()->DumpToString(true));
    }
    out->push_back(status_pb);
  }
}

Result<OpId> TabletPeer::MaxPersistentOpId() const {
  auto tablet = shared_tablet_maybe_null();
  if (!tablet) {
    // Tablet peer not yet initialized -- we could be doing tablet bootstrap still.
    return OpId::Min();
  }
  auto persistent_state = VERIFY_RESULT(tablet->GetPersistentState());
  auto result = OpId::MinValid(persistent_state.intents, persistent_state.regular);
  for (const auto& vector_index : persistent_state.vector_indexes) {
    result = OpId::MinValid(result, vector_index.op_id);
  }
  return result;
}

Result<log::MinRetainLogIndexInfo> TabletPeer::GetEarliestNeededLogIndex(
    std::string* retention_details) const {
  if (PREDICT_FALSE(!log_)) {
    auto status = STATUS(Uninitialized, "Log not ready (tablet peer not yet initialized?)");
    LOG(DFATAL) << status;
    return status;
  }

  std::vector<std::string> factors;
  const auto AddIndexFactor =
      [&factors, retention_details](
          const std::string& name, int64_t index, const std::string& extra = "") {
    if (retention_details) {
      factors.push_back(Format(
          "$0: $1$2", name,
          index == std::numeric_limits<int64_t>::max() ? "<max_int>" : std::to_string(index),
          extra));
    }
  };
  const auto FinalizeRetentionDetails = [&factors, retention_details](int64_t result_index) {
    if (retention_details) {
      *retention_details += Format(
          "Earliest needed op ID idx = min of [$0] = $1.",
          JoinStrings(factors, ", "), result_index);
    }
  };

  // First, we anchor on the last OpId in the Log to establish a lower bound
  // and avoid racing with the other checks. This limits the Log GC candidate
  // segments before we check the anchors.
  auto latest_log_entry_op_id = log_->GetLatestEntryOpId();
  int64_t min_index = latest_log_entry_op_id.index;
  AddIndexFactor("latest log entry op ID idx", latest_log_entry_op_id.index);

  // If we never have written to the log, no need to proceed.
  if (min_index == 0) {
    FinalizeRetentionDetails(min_index);
    return log::MinRetainLogIndexInfo{min_index};
  }

  // Next, we interrogate the anchor registry.
  // Returns OK if minimum known, NotFound if no anchors are registered.
  {
    int64_t min_anchor_index;
    Status s = log_anchor_registry_->GetEarliestRegisteredLogIndex(&min_anchor_index);
    if (PREDICT_FALSE(!s.ok())) {
      DCHECK(s.IsNotFound()) << "Unexpected error calling LogAnchorRegistry: " << s.ToString();
    } else {
      min_index = std::min(min_index, min_anchor_index);
      AddIndexFactor("min anchor idx", min_anchor_index);
    }
  }

  // Next, interrogate the OperationTracker.
  int64_t min_pending_op_index = std::numeric_limits<int64_t>::max();
  for (const auto& driver : operation_tracker_.GetPendingOperations()) {
    auto tx_op_id = driver->GetOpId();
    // A operation which doesn't have an opid hasn't been submitted for replication yet and
    // thus has no need to anchor the log.
    if (tx_op_id != yb::OpId::Invalid()) {
      min_pending_op_index = std::min(min_pending_op_index, tx_op_id.index);
    }
  }

  min_index = std::min(min_index, min_pending_op_index);
  if (min_pending_op_index != std::numeric_limits<int64_t>::max()) {
    AddIndexFactor("min pending op ID idx", min_pending_op_index);
  }

  auto min_retryable_request_op_id = VERIFY_RESULT(GetRaftConsensus())->MinRetryableRequestOpId();
  min_index = std::min(min_index, min_retryable_request_op_id.index);
  AddIndexFactor("min retryable req op ID idx", min_retryable_request_op_id.index);

  auto tablet = VERIFY_RESULT(shared_tablet());
  auto* transaction_coordinator = tablet->transaction_coordinator();
  if (transaction_coordinator) {
    std::string txn_coord_detail;
    auto transaction_coordinator_min_op_index =
        transaction_coordinator->PrepareGC(retention_details ? &txn_coord_detail : nullptr);
    min_index = std::min(min_index, transaction_coordinator_min_op_index);
    if (transaction_coordinator_min_op_index != std::numeric_limits<int64_t>::max()) {
      AddIndexFactor(
          "txn coord min op ID idx", transaction_coordinator_min_op_index,
          Format(" ($0)", txn_coord_detail));
    }
  }

  // We keep at least one committed operation in the log so that we can always recover safe time
  // during bootstrap.
  // Last committed op id should be read before Tablet::GetPersistentState to avoid race condition
  // described in MaxPersistentOpIdForDb.
  //
  // If we read last committed op id AFTER reading last persistent op id (INCORRECT):
  // - We read max persistent op id and find there is no new data, so we ignore it.
  // - New data gets written and Raft-committed, but not yet flushed to an SSTable.
  // - We read the last committed op id, which is greater than what max persistent op id would have
  //   returned.
  // - We garbage-collect the Raft log entries corresponding to the new data.
  // - Power is lost and the server reboots, losing committed data.
  //
  // If we read last committed op id BEFORE reading last persistent op id (CORRECT):
  // - We read the last committed / majority-replicated / PRE_VOTER retention op ids.
  // - We read max persistent op id and find there is no new data, so we ignore it.
  // - New data gets written and Raft-committed, but not yet flushed to an SSTable.
  // - We still don't garbage-collect the logs containing the committed but unflushed data,
  //   because the earlier value of the last committed op id that we read prevents us from doing so.
  auto wal_gc_retention_info = VERIFY_RESULT(GetRaftConsensus())->GetWalGcRetentionOpIdInfo();
  min_index = std::min(min_index, wal_gc_retention_info.committed_op_id.index);
  AddIndexFactor("last committed op ID idx", wal_gc_retention_info.committed_op_id.index);
  min_index = std::min(min_index, wal_gc_retention_info.majority_replicated_op_id.index);
  AddIndexFactor(
      "majority replicated op ID idx",
      wal_gc_retention_info.majority_replicated_op_id.index);
  min_index = std::min(min_index, wal_gc_retention_info.min_progressing_pre_voter_op_id.index);
  AddIndexFactor(
      "min progressing PRE_VOTER op ID idx",
      wal_gc_retention_info.min_progressing_pre_voter_op_id.index);

  if (tablet_->table_type() != TableType::TRANSACTION_STATUS_TABLE_TYPE) {
    min_index = std::min(
        min_index,
        VERIFY_RESULT(tablet_->EarliestNeededLogIndex(latest_log_entry_op_id, AddIndexFactor)));
  }

  if (meta_->IsLazySuperblockFlushEnabled()) {
    // Unapplied change metadata operations, if any, are taken into account above. The below
    // takes into accounts any applied but unflushed change metadata operations.

    // TODO(lazy_sb_flush): MinUnflushedChangeMetadataOpId() requires flush_lock_ which can be
    // expensive to get during a superblock flush. Get rid of the below logic, if possible, post
    // https://github.com/yugabyte/yugabyte-db/issues/16684.
    auto min_unflushed_change_metadata_index = meta_->MinUnflushedChangeMetadataOpId().index;
    min_index = std::min(min_index, min_unflushed_change_metadata_index);
    AddIndexFactor(
        "min unflushed CHANGE_METADATA_OP ID idx", min_unflushed_change_metadata_index);
  }

  FinalizeRetentionDetails(min_index);

  // Index xrepl (CDCSDK/xCluster) still needs the source to retain. This is the same value Log GC
  // applies as its soft xrepl floor; bundling it here gives remote bootstrap and GC one source of
  // truth. Returns int64 max when no xrepl consumer constrains retention.
  std::string xrepl_factors;
  const int64_t log_index_needed_by_cdc =
      log_->GetXReplMinReplicatedIndex(retention_details ? &xrepl_factors : nullptr);
  if (retention_details && log_index_needed_by_cdc != std::numeric_limits<int64_t>::max()) {
    *retention_details += Format(
        " log_index_needed_by_cdc = min of [$0] = $1.", xrepl_factors,
        log_index_needed_by_cdc);
  }

  return log::MinRetainLogIndexInfo{min_index, log_index_needed_by_cdc};
}

Result<std::pair<OpId, HybridTime>> TabletPeer::GetOpIdAndSafeTimeForXReplBootstrap() const {
  auto tablet = VERIFY_RESULT(shared_tablet());

  SCHECK_NE(
      tablet->table_type(), TableType::TRANSACTION_STATUS_TABLE_TYPE, IllegalState,
      "Transaction status table cannot be bootstrapped.");

  auto op_id = GetLatestLogEntryOpId();

  // The bootstrap_time is the minium time from which the provided OpId will be transactionally
  // consistent. It is important to call AbortActiveTransactions, which resolves the pending
  // transactions and aborts the active ones. This step synchronizes our clock with the
  // transaction status tablet clock, ensuring that the bootstrap_time we compute later is correct.
  // Ex: Our safe time is 100, and we have a pending intent for which the log got GCed. So this
  // transaction cannot be replicated. If the transaction is still active it needs to be aborted.
  // If, the coordinator is at 110 and the transaction was committed at 105. We need to move our
  // clock to 110 and pick a higher bootstrap_time so that the commit is not part of the bootstrap.
  if (FLAGS_abort_active_txns_during_xrepl_bootstrap) {
    AbortActiveTransactions();
  }
  auto bootstrap_time = VERIFY_RESULT(tablet->SafeTime(RequireLease::kTrue));
  return std::make_pair(std::move(op_id), std::move(bootstrap_time));
}

Status TabletPeer::GetGCableDataSize(int64_t* retention_size) const {
  RETURN_NOT_OK(CheckRunning());

  const int64_t diag_min_retain_age_secs = FLAGS_log_retention_diagnostics_min_age_secs;
  bool collect_diagnostics = false;
  if (diag_min_retain_age_secs > 0) {
    constexpr auto kLogInterval = std::chrono::minutes(1);
    const auto now = CoarseMonoClock::Now();
    auto next_allowed = next_wal_retention_diag_log_time_.load();
    collect_diagnostics =
        now >= next_allowed &&
        next_wal_retention_diag_log_time_.compare_exchange_strong(next_allowed, now + kLogInterval);
  }

  if (!collect_diagnostics) {
    const auto min_op_idx = VERIFY_RESULT(GetEarliestNeededLogIndex());
    return log_->GetGCableDataSize(min_op_idx, retention_size);
  }

  log::WalRetentionDiagnostics diagnostics;
  const auto min_op_idx = VERIFY_RESULT(GetEarliestNeededLogIndex(&diagnostics.details));
  RETURN_NOT_OK(log_->GetGCableDataSize(min_op_idx, retention_size, &diagnostics));

  if (diagnostics.first_retained_segment_age_secs >= diag_min_retain_age_secs &&
      !diagnostics.details.empty()) {
    LOG(DETAIL) << LogPrefix()
        << "WAL retention details (gcable_bytes=" << *retention_size
        << ", first_retain_seg_age=" << diagnostics.first_retained_segment_age_secs << "s): "
        << diagnostics.details;
  }
  return Status::OK();
}

log::Log* TabletPeer::log() const {
  Log* log = log_atomic_.load(std::memory_order_acquire);
  LOG_IF_WITH_PREFIX(FATAL, !log) << "log() called before the log instance is initialized.";
  return log;
}

yb::OpId TabletPeer::GetLatestLogEntryOpId() const {
  Log* log = log_atomic_.load(std::memory_order_acquire);
  if (log) {
    return log->GetLatestEntryOpId();
  }
  return yb::OpId();
}

bool TabletPeer::is_cdc_barrier_stale(
    const MonoTime& refresh_time, double* seconds_since_last_refresh_ptr) const {
  auto seconds_since_last_refresh = MonoTime::Now().GetDeltaSince(refresh_time).ToSeconds();
  if (seconds_since_last_refresh_ptr) {
    *seconds_since_last_refresh_ptr = seconds_since_last_refresh;
  }
  auto stale_secs = meta_->IsSysCatalog()
      ? FLAGS_cdc_min_replicated_index_considered_stale_secs_master
      : FLAGS_cdc_min_replicated_index_considered_stale_secs;
  return (seconds_since_last_refresh > stale_secs);
}

bool TabletPeer::is_cdc_min_replicated_index_stale(double* seconds_since_last_refresh_ptr) const {
  std::lock_guard l(cdc_resource_refresh_time_lock_);
  return is_cdc_barrier_stale(
      cdc_min_replicated_index_refresh_time_, seconds_since_last_refresh_ptr);
}

bool TabletPeer::is_cdc_sdk_safe_time_stale(double* seconds_since_last_refresh_ptr) const {
  std::lock_guard l(cdc_resource_refresh_time_lock_);
  return is_cdc_barrier_stale(cdc_sdk_safe_time_refresh_time_, seconds_since_last_refresh_ptr);
}

Status TabletPeer::set_cdc_min_replicated_index_unlocked(int64_t cdc_min_replicated_index) {
  VLOG(1) << "Setting cdc min replicated index to " << cdc_min_replicated_index;
  RETURN_NOT_OK(meta_->set_cdc_min_replicated_index(cdc_min_replicated_index));
  Log* log = log_atomic_.load(std::memory_order_acquire);
  if (log) {
    log->set_cdc_min_replicated_index(cdc_min_replicated_index);
  }
  cdc_min_replicated_index_refresh_time_ = MonoTime::Now();
  return Status::OK();
}

Status TabletPeer::set_cdc_min_replicated_index(int64_t cdc_min_replicated_index) {
  std::lock_guard l(cdc_resource_refresh_time_lock_);
  return set_cdc_min_replicated_index_unlocked(cdc_min_replicated_index);
}

Status TabletPeer::reset_cdc_min_replicated_index_if_stale() {
  double seconds_since_last_refresh;
  if (is_cdc_min_replicated_index_stale(&seconds_since_last_refresh)) {
    LOG_WITH_PREFIX(INFO) << "Resetting cdc min replicated index. Seconds since last update: "
                          << seconds_since_last_refresh;
    RETURN_NOT_OK(set_cdc_min_replicated_index(std::numeric_limits<int64_t>::max()));
  }
  return Status::OK();
}

int64_t TabletPeer::get_cdc_min_replicated_index() {
  return meta_->cdc_min_replicated_index();
}

Status TabletPeer::set_cdc_sdk_min_checkpoint_op_id(const OpId& cdc_sdk_min_checkpoint_op_id) {
  VLOG(1) << "Setting CDCSDK min checkpoint opId to " << cdc_sdk_min_checkpoint_op_id.ToString();
  RETURN_NOT_OK(meta_->set_cdc_sdk_min_checkpoint_op_id(cdc_sdk_min_checkpoint_op_id));
  return Status::OK();
}

Status TabletPeer::set_cdc_sdk_safe_time(const HybridTime& cdc_sdk_safe_time) {
  VLOG(1) << "Setting CDCSDK safe time to " << cdc_sdk_safe_time;
  RETURN_NOT_OK(meta_->set_cdc_sdk_safe_time(cdc_sdk_safe_time));
  {
    std::lock_guard l(cdc_resource_refresh_time_lock_);
    cdc_sdk_safe_time_refresh_time_ = MonoTime::Now();
  }
  return Status::OK();
}

HybridTime TabletPeer::get_cdc_sdk_safe_time() {
  return meta_->cdc_sdk_safe_time();
}

OpId TabletPeer::cdc_sdk_min_checkpoint_op_id() {
  return meta_->cdc_sdk_min_checkpoint_op_id();
}

CoarseTimePoint TabletPeer::cdc_sdk_min_checkpoint_op_id_expiration() {
  auto tablet = shared_tablet_maybe_null();
  if (tablet) {
    auto txn_participant = tablet->transaction_participant();
    if (txn_participant) {
      return txn_participant->GetCheckpointExpirationTime();
    }
  }

  return CoarseTimePoint();
}

bool TabletPeer::is_under_cdc_sdk_replication() {
  return meta_->is_under_cdc_sdk_replication();
}

Result<CDCRetentionBarrierMoveSelector> TabletPeer::reset_cdc_retention_barriers_if_stale() {
  double seconds_since_cdc_min_replicated_index_last_refresh;
  double seconds_since_cdc_sdk_safe_time_last_refresh;
  bool is_cdc_min_replicated_index_stale =
      this->is_cdc_min_replicated_index_stale(&seconds_since_cdc_min_replicated_index_last_refresh);
  bool is_cdc_sdk_safe_time_stale =
      this->is_cdc_sdk_safe_time_stale(&seconds_since_cdc_sdk_safe_time_last_refresh);
  if (!is_cdc_min_replicated_index_stale && !is_cdc_sdk_safe_time_stale) {
    return CDCRetentionBarrierMoveSelector{
        .move_cdc_min_replicated_index = false,
        .move_cdc_sdk_min_checkpoint_op_id = false,
        .move_cdc_sdk_safe_time = false};
  }

  // The WAL and intent retention barriers share the cdc_min_replicated_index staleness clock, so
  // when it is stale we release both of them. The history barrier has its own clock and is released
  // independently. Barriers that are still being advanced are left in place.
  CDCRetentionBarrierMoveSelector barrier_move_selector{
      .move_cdc_min_replicated_index = is_cdc_min_replicated_index_stale,
      .move_cdc_sdk_min_checkpoint_op_id = is_cdc_min_replicated_index_stale,
      .move_cdc_sdk_safe_time = is_cdc_sdk_safe_time_stale};
  VLOG_WITH_PREFIX(1) << "Trying to reset stale cdc retention barriers. WAL/intent barriers stale: "
                      << is_cdc_min_replicated_index_stale << " ("
                      << seconds_since_cdc_min_replicated_index_last_refresh
                      << "s), history barrier stale: " << is_cdc_sdk_safe_time_stale << " ("
                      << seconds_since_cdc_sdk_safe_time_last_refresh << "s)";
  RETURN_NOT_OK(SetAllCDCRetentionBarriers(
      std::numeric_limits<int64_t>::max() /* cdc_wal_index */,
      OpId::Max() /* cdc_sdk_intents_op_id */, MonoDelta::kZero /* cdc_sdk_op_id_expiration */,
      HybridTime::kInvalid /* cdc_sdk_history_cutoff */, true /* require_history_cutoff */,
      false /* initial_retention_barrier */, barrier_move_selector));
  TEST_SYNC_POINT("TabletPeer::reset_cdc_retention_barriers_if_stale::End");
  return barrier_move_selector;
}

OpId TabletPeer::GetLatestCheckPoint() {
  auto tablet = shared_tablet_maybe_null();
  if (tablet) {
    auto txn_participant = tablet->transaction_participant();
    if (txn_participant) {
      return txn_participant->GetLatestCheckPoint();
    }
  }
  return OpId();
}

Result<NamespaceId> TabletPeer::GetNamespaceId() {
  auto tablet = VERIFY_RESULT(shared_tablet());
  auto& tablet_metadata = *tablet->metadata();
  RETURN_NOT_OK(BackfillNamespaceIdIfNeeded(
      tablet_metadata.table_id(), tablet_metadata, *client_future().get()));
  auto namespace_id = tablet->metadata()->namespace_id();
  RSTATUS_DCHECK(
      !namespace_id.empty(), IllegalState, "Namespace ID is empty for tablet $0",
      tablet->tablet_id());
  return namespace_id;
}

HybridTime TabletPeer::GetMinStartHTRunningTxnsOrLeaderSafeTime() {
  auto tablet_result = shared_tablet();
  if (!tablet_result.ok()) {
    LOG_WITH_PREFIX(WARNING)
        << "Tablet not found, so setting minimum start hybrid time for running txns to kInitial.";
    return HybridTime::kInitial;
  }
  return (*tablet_result)->GetMinStartHTRunningTxnsForCDCLogCallback();
}

Status TabletPeer::SetCDCSDKRetainOpIdAndTime(
    const OpId& cdc_sdk_op_id, const MonoDelta& cdc_sdk_op_id_expiration,
    const HybridTime& cdc_sdk_safe_time) {
  RETURN_NOT_OK(set_cdc_sdk_min_checkpoint_op_id(cdc_sdk_op_id));
  RETURN_NOT_OK(set_cdc_sdk_safe_time(cdc_sdk_safe_time));

  {
    std::lock_guard lock(lock_);
    RETURN_NOT_OK(CheckRunning());
    auto txn_participant = tablet_->transaction_participant();
    if (txn_participant) {
      auto log = log_atomic_.load(std::memory_order_acquire);
      auto min_start_ht_cdc_unstreamed_txns = tablet_->GetMinStartHTCDCUnstreamedTxns(log);
      VLOG_WITH_PREFIX_AND_FUNC(1)
          << "Intents opid retention duration = " << cdc_sdk_op_id_expiration
          << ", Minimum start time for CDC unstreamed txns from available gc log segments = "
          << min_start_ht_cdc_unstreamed_txns;

      txn_participant->SetIntentRetainOpIdAndTime(
          cdc_sdk_op_id, cdc_sdk_op_id_expiration, min_start_ht_cdc_unstreamed_txns);
      if (FLAGS_cdc_immediate_transaction_cleanup) {
        tablet_->CleanupIntentFiles();
      }
    }
  }
  return Status::OK();
}

Result<MonoDelta> TabletPeer::GetCDCSDKIntentRetainTime(const int64_t& cdc_sdk_latest_active_time) {
  MonoDelta cdc_sdk_intent_retention = MonoDelta::kZero;
  // If cdc_sdk_latest_update_time is not updated to default CoarseTimePoint::min() value,
  // It's mean that, no need to retain the intents. This can happen in below case:-
  //      a. Only XCluster streams are defined for the tablet.
  //      b. CDCSDK stream for the tablet is expired.
  if (cdc_sdk_latest_active_time == 0) {
    return cdc_sdk_intent_retention;
  }

  auto txn_participant = VERIFY_RESULT(shared_tablet())->transaction_participant();
  if (txn_participant) {
    // Get the current tablet LEADER's intent retention expiration time.
    // Check how many milliseconds time remaining w.r.t current time, update
    // all the FOLLOWERs as their cdc_sdk_min_checkpoint_op_id_expiration_.
    MonoDelta max_retain_time =
        MonoDelta::FromMilliseconds(FLAGS_cdc_intent_retention_ms);
    auto lastest_active_time =
        MonoDelta::FromMicroseconds(GetCurrentTimeMicros() - cdc_sdk_latest_active_time);
    if (max_retain_time >= lastest_active_time) {
      cdc_sdk_intent_retention = max_retain_time - lastest_active_time;
    }
  }
  return cdc_sdk_intent_retention;
}

Result<bool> TabletPeer::SetAllCDCRetentionBarriers(
    int64 cdc_wal_index, OpId cdc_sdk_intents_op_id, MonoDelta cdc_sdk_op_id_expiration,
    HybridTime cdc_sdk_history_cutoff, bool require_history_cutoff, bool initial_retention_barrier,
    CDCRetentionBarrierMoveSelector barrier_move_selector) {
  auto tablet = VERIFY_RESULT(shared_tablet());
  Log* log = log_atomic_.load(std::memory_order_acquire);

  {
    std::lock_guard lock(cdc_resource_refresh_time_lock_);
    auto now = MonoTime::Now();
    // Refresh only the clocks of the barriers actually being moved.
    if (barrier_move_selector.move_cdc_min_replicated_index ||
        barrier_move_selector.move_cdc_sdk_min_checkpoint_op_id) {
      cdc_min_replicated_index_refresh_time_ = now;
    }
    if (barrier_move_selector.move_cdc_sdk_safe_time) {
      cdc_sdk_safe_time_refresh_time_ = now;
    }
  }

  if (initial_retention_barrier) {
    RETURN_NOT_OK(tablet->SetAllInitialCDCRetentionBarriers(
        log, cdc_wal_index, cdc_sdk_intents_op_id, cdc_sdk_history_cutoff, require_history_cutoff,
        barrier_move_selector));
    return true;
  } else {
    return tablet->MoveForwardAllCDCRetentionBarriers(
        log, cdc_wal_index, cdc_sdk_intents_op_id, cdc_sdk_op_id_expiration, cdc_sdk_history_cutoff,
        require_history_cutoff, barrier_move_selector);
  }
}

// Applies to both CDCSDK and XCluster streams attempting to set their initial
// retention barrier
Result<bool> TabletPeer::SetAllInitialCDCRetentionBarriers(
    int64 cdc_wal_index, OpId cdc_sdk_intents_op_id, HybridTime cdc_sdk_history_cutoff,
    bool require_history_cutoff, CDCRetentionBarrierMoveSelector barrier_move_selector) {
  MonoDelta cdc_sdk_op_id_expiration =
      MonoDelta::FromMilliseconds(FLAGS_cdc_intent_retention_ms);
  return SetAllCDCRetentionBarriers(
      cdc_wal_index, cdc_sdk_intents_op_id, cdc_sdk_op_id_expiration, cdc_sdk_history_cutoff,
      require_history_cutoff, /*initial_retention_barrier=*/true, barrier_move_selector);
}

// Applies only to CDCSDK streams
Result<bool> TabletPeer::SetAllInitialCDCSDKRetentionBarriers(
    OpId cdc_sdk_op_id, HybridTime cdc_sdk_history_cutoff, bool require_history_cutoff) {

  return SetAllInitialCDCRetentionBarriers(
      cdc_sdk_op_id.index, cdc_sdk_op_id, cdc_sdk_history_cutoff, require_history_cutoff);
}

// Applies to the combined requirement of both CDCSDK and XCluster streams.
// UpdatePeersAndMetrics will call this with the strictest requirements
// corresponding to the slowest consumer of this tablet among all streams.
Result<bool> TabletPeer::MoveForwardAllCDCRetentionBarriers(
    int64 cdc_wal_index, OpId cdc_sdk_intents_op_id, MonoDelta cdc_sdk_op_id_expiration,
    HybridTime cdc_sdk_history_cutoff, bool require_history_cutoff,
    CDCRetentionBarrierMoveSelector barrier_move_selector) {
  return SetAllCDCRetentionBarriers(
      cdc_wal_index, cdc_sdk_intents_op_id, cdc_sdk_op_id_expiration, cdc_sdk_history_cutoff,
      require_history_cutoff, /*initial_retention_barrier=*/false, barrier_move_selector);
}

std::string TabletPeer::AllCDCRetentionBarriersToString() const {
  return tablet_metadata()->AllCDCRetentionBarriersToString();
}

std::unique_ptr<Operation> TabletPeer::CreateOperation(consensus::LWReplicateMsg* replicate_msg) {
  // TODO: handle cases where tablet is unset safely.
  auto tablet = CHECK_RESULT(shared_tablet());
  switch (replicate_msg->op_type()) {
    case consensus::WRITE_OP:
      DCHECK(replicate_msg->has_write()) << "WRITE_OP replica"
          " operation must receive a WriteRequestPB";
      // We use separate preparing token only on leader, so here it could be empty.
      return std::make_unique<WriteOperation>(tablet);

    case consensus::CHANGE_METADATA_OP:
      DCHECK(replicate_msg->has_change_metadata_request()) << "CHANGE_METADATA_OP replica"
          " operation must receive an ChangeMetadataRequestPB";
      return std::make_unique<ChangeMetadataOperation>(tablet, log());

    case consensus::UPDATE_TRANSACTION_OP:
      DCHECK(replicate_msg->has_transaction_state()) << "UPDATE_TRANSACTION_OP replica"
          " operation must receive an TransactionStatePB";
      return std::make_unique<UpdateTxnOperation>(tablet);

    case consensus::TRUNCATE_OP:
      DCHECK(replicate_msg->has_truncate()) << "TRUNCATE_OP replica"
          " operation must receive an TruncateRequestPB";
      return std::make_unique<TruncateOperation>(tablet);

    case consensus::SNAPSHOT_OP:
       DCHECK(replicate_msg->has_snapshot_request()) << "SNAPSHOT_OP replica"
          " operation must receive an TabletSnapshotOpRequestPB";
      return std::make_unique<SnapshotOperation>(tablet);

    case consensus::HISTORY_CUTOFF_OP:
       DCHECK(replicate_msg->has_history_cutoff()) << "HISTORY_CUTOFF_OP replica"
          " transaction must receive an HistoryCutoffPB";
      return std::make_unique<HistoryCutoffOperation>(tablet);

    case consensus::SPLIT_OP:
       DCHECK(replicate_msg->has_split_request()) << "SPLIT_OP replica"
          " operation must receive an SplitOpRequestPB";
      return std::make_unique<SplitOperation>(tablet, tablet_splitter_);

    case consensus::CHANGE_AUTO_FLAGS_CONFIG_OP:
      DCHECK(replicate_msg->has_auto_flags_config())
          << "CHANGE_AUTO_FLAGS_CONFIG_OP replica"
             " operation must receive an AutoFlagsConfigPB";
      return std::make_unique<ChangeAutoFlagsConfigOperation>(
          tablet, replicate_msg->mutable_auto_flags_config());

    case consensus::CLONE_OP:
       DCHECK(replicate_msg->has_clone_tablet()) << "CLONE_OP replica"
          " operation must receive a CloneOpRequestPB";
      return std::make_unique<CloneOperation>(tablet, tablet_splitter_);

    case consensus::UNKNOWN_OP: FALLTHROUGH_INTENDED;
    case consensus::NO_OP: FALLTHROUGH_INTENDED;
    case consensus::CHANGE_CONFIG_OP:
      break;
  }
  LOG(DFATAL) << "Invalid replicate message: " << replicate_msg->ShortDebugString();
  return nullptr;
}

Status TabletPeer::StartReplicaOperation(
    const scoped_refptr<ConsensusRound>& round, HybridTime propagated_safe_time) {
  RaftGroupStatePB value = state();
  if (value != RaftGroupStatePB::RUNNING && value != RaftGroupStatePB::BOOTSTRAPPING) {
    return STATUS(IllegalState, RaftGroupStatePB_Name(value));
  }

  auto* replicate_msg = round->replicate_msg().get();
  DCHECK(replicate_msg->has_hybrid_time());
  auto operation = CreateOperation(replicate_msg);
  if (!operation) {
    auto status = STATUS_FORMAT(InvalidArgument, "Wrong operation: $0", round->ToString());
    LOG(DFATAL) << status;
    return status;
  }

  // TODO(todd) Look at wiring the stuff below on the driver
  // It's imperative that we set the round here on any type of operation, as this
  // allows us to keep the reference to the request in the round instead of copying it.
  operation->set_consensus_round(round);
  HybridTime ht(replicate_msg->hybrid_time());
  operation->set_hybrid_time(ht);
  clock_->Update(ht);

  // This sets the monotonic counter to at least replicate_msg.monotonic_counter() atomically.
  tablet_->UpdateMonotonicCounter(replicate_msg->monotonic_counter());

  auto* operation_ptr = operation.get();
  OperationDriverPtr driver = VERIFY_RESULT(NewReplicaOperationDriver(&operation));

  operation_ptr->consensus_round()->SetCallback(driver.get());

  if (propagated_safe_time) {
    driver->SetPropagatedSafeTime(propagated_safe_time, tablet_->mvcc_manager());
  }

  driver->ExecuteAsync();
  return Status::OK();
}

void TabletPeer::SetPropagatedSafeTime(HybridTime ht) {
  auto driver = NewReplicaOperationDriver(nullptr);
  if (!driver.ok()) {
    LOG_WITH_PREFIX(DFATAL) << "Failed to create operation driver to set propagated hybrid time";
    return;
  }
  (**driver).SetPropagatedSafeTime(ht, tablet_->mvcc_manager());
  (**driver).ExecuteAsync();
}

void TabletPeer::SetMvccPropagatedSafeTime(HybridTime ht) {
  tablet_->mvcc_manager()->SetPropagatedSafeTimeOnFollower(ht);
}

bool TabletPeer::ShouldApplyWrite() {
  return tablet_->ShouldApplyWrite();
}

bool TabletPeer::AreWritesStopped() {
  auto tablet = shared_tablet_maybe_null();
  return tablet && tablet->AreWritesStopped();
}

Result<std::shared_ptr<consensus::Consensus>> TabletPeer::GetConsensus() const {
  return GetRaftConsensus();
}

Result<shared_ptr<consensus::RaftConsensus>> TabletPeer::GetRaftConsensus() const {
  std::lock_guard lock(lock_);
  if (PREDICT_FALSE(FLAGS_TEST_tablet_peer_force_get_raft_consensus_failure)) {
    return STATUS_FORMAT(
        IllegalState, "$0FLAGS_TEST_tablet_peer_force_get_raft_consensus_failure is set",
        LogPrefix());
  }
  // Cannot use NotFound status for the shutting down case as later the status may be extended with
  // TabletServerErrorPB::TABLET_NOT_RUNNING error code, and this combination of the status and
  // the code is not expected and is not considered as a retryable operation at least by yb-client.
  // Refer to https://github.com/yugabyte/yugabyte-db/issues/19033 for the details.
  SCHECK(!IsShutdownStarted(), IllegalState, "Tablet peer $0 is shutting down", LogPrefix());
  SCHECK(consensus_, IllegalState, "Tablet peer $0 is not started yet", LogPrefix());
  return consensus_;
}

shared_ptr<consensus::RaftConsensus> TabletPeer::GetRaftConsensusUnsafe() const {
  std::lock_guard lock(lock_);
  return consensus_;
}

std::shared_ptr<TabletBootstrapStateFlusher> TabletPeer::shared_bootstrap_state_flusher() const {
  std::lock_guard lock(lock_);
  return bootstrap_state_flusher_;
}

Result<OperationDriverPtr> TabletPeer::NewLeaderOperationDriver(
    std::unique_ptr<Operation>* operation, int64_t term) {
  if (term == OpId::kUnknownTerm) {
    return STATUS(InvalidArgument, "Leader operation driver for unknown term");
  }
  return NewOperationDriver(operation, term);
}

Result<OperationDriverPtr> TabletPeer::NewReplicaOperationDriver(
    std::unique_ptr<Operation>* operation) {
  return NewOperationDriver(operation, OpId::kUnknownTerm);
}

Result<OperationDriverPtr> TabletPeer::NewOperationDriver(std::unique_ptr<Operation>* operation,
                                                          int64_t term) {
  auto operation_driver = CreateOperationDriver();
  RETURN_NOT_OK(operation_driver->Init(operation, term));
  return operation_driver;
}

void TabletPeer::RegisterMaintenanceOps(MaintenanceManager* maint_mgr) {
  // Taking state_change_lock_ ensures that we don't shut down concurrently with
  // this last start-up task.
  // Note that the state_change_lock_ is taken in Shutdown(),
  // prior to calling UnregisterMaintenanceOps().

  std::lock_guard l(state_change_lock_);

  if (state() != RaftGroupStatePB::RUNNING) {
    LOG_WITH_PREFIX(WARNING) << "Not registering maintenance operations: tablet not RUNNING";
    return;
  }

  DCHECK(maintenance_ops_.empty());

  auto tablet_result = shared_tablet();
  if (!tablet_result.ok()) {
    LOG_WITH_PREFIX(WARNING) << "Not registering maintenance operations: "
                             << tablet_result.status();
    return;
  }
  auto log_gc = std::make_unique<LogGCOp>(this, *tablet_result);
  maint_mgr->RegisterOp(log_gc.get());
  maintenance_ops_.push_back(std::move(log_gc));
  LOG_WITH_PREFIX(INFO) << "Registered log gc";

  auto reset_stale_retention_barriers_op =
      std::make_unique<ResetStaleRetentionBarriersOp>(this, *tablet_result);
  maint_mgr->RegisterOp(reset_stale_retention_barriers_op.get());
  maintenance_ops_.push_back(std::move(reset_stale_retention_barriers_op));
  LOG_WITH_PREFIX(INFO) << "Registered stale retention barrier resetter";
}

void TabletPeer::UnregisterMaintenanceOps() {
  DCHECK(state_change_lock_.is_locked());
  for (auto& op : maintenance_ops_) {
    op->Unregister();
  }
  maintenance_ops_.clear();
}

TabletOnDiskSizeInfo TabletPeer::GetOnDiskSizeInfo() const {
  std::lock_guard lock(lock_);
  return GetOnDiskSizeInfoUnlocked();
}

TabletOnDiskSizeInfo TabletPeer::GetOnDiskSizeInfoUnlocked() const {
  TabletOnDiskSizeInfo info;

  if (consensus_) {
    info.consensus_metadata_disk_size = consensus_->OnDiskSize();
  }

  if (tablet_) {
    info.sst_files_disk_size = tablet_->GetCurrentVersionSstFilesSize();
    info.uncompressed_sst_files_disk_size =
        tablet_->GetCurrentVersionSstFilesUncompressedSize();
    info.vector_index_disk_size = tablet_->vector_indexes().List().OnDiskSize();
  }

  auto log = log_atomic_.load(std::memory_order_acquire);
  if (log) {
    info.wal_files_disk_size = log->OnDiskSize();
  }

  info.total_on_disk_size = total_on_disk_size_;
  info.RecomputeTotalSize();
  return info;
}

void TabletPeer::SetTabletOnDiskSize(size_t total_on_disk_size) {
  total_on_disk_size_ = total_on_disk_size;
}

size_t TabletPeer::GetNumLogSegments() const {
  auto log = log_atomic_.load(std::memory_order_acquire);
  return log ? log->num_segments() : 0;
}

std::string TabletPeer::LogPrefix() const {
  return Format("T $0 P $1 [state=$2]: ",
      tablet_id_, permanent_uuid_, RaftGroupStatePB_Name(state()));
}

scoped_refptr<OperationDriver> TabletPeer::CreateOperationDriver() {
  return scoped_refptr<OperationDriver>(new OperationDriver(
      &operation_tracker_,
      GetRaftConsensusUnsafe().get(),
      prepare_thread_.get(),  // May be nullptr
      tablet_->table_type()));
}

Result<client::YBClient*> TabletPeer::client() const {
  auto cached_value = client_cache_.load(std::memory_order_acquire);
  if (cached_value != nullptr) {
    return cached_value;
  }
  auto future_status = client_future_.wait_for(
      TransactionRpcTimeout().ToSteadyDuration());
  if (future_status != std::future_status::ready) {
    return STATUS(TimedOut, "Client not ready");
  }
  auto result = client_future_.get();
  client_cache_.store(result, std::memory_order_release);
  return result;
}

int64_t TabletPeer::LeaderTerm() const {
  auto consensus = GetRaftConsensusUnsafe();
  return consensus ? consensus->LeaderTerm() : yb::OpId::kUnknownTerm;
}

Result<HybridTime> TabletPeer::LeaderSafeTime() const {
  return tablet_->SafeTime();
}

consensus::LeaderStatus TabletPeer::LeaderStatus(bool allow_stale) const {
  auto consensus = GetRaftConsensusUnsafe();
  return consensus ? consensus->GetLeaderStatus(allow_stale) : consensus::LeaderStatus::NOT_LEADER;
}

bool TabletPeer::IsLeaderAndReady() const {
  return LeaderStatus() == consensus::LeaderStatus::LEADER_AND_READY;
}

bool TabletPeer::IsNotLeader() const {
  return LeaderStatus() == consensus::LeaderStatus::NOT_LEADER;
}

Result<HybridTime> TabletPeer::HtLeaseExpiration() const {
  auto consensus = VERIFY_RESULT(GetRaftConsensus());
  HybridTime result(
      CHECK_RESULT(consensus->MajorityReplicatedHtLeaseExpiration(0, CoarseTimePoint::max())), 0);
  return std::max(result, tablet_->mvcc_manager()->LastReplicatedHybridTime());
}

TableType TabletPeer::TEST_table_type() {
  auto tablet_result = shared_tablet();
  if (!tablet_result.ok()) {
    LOG(WARNING) << "Cannot determine table type: " << tablet_result.status();
    return static_cast<TableType>(-1);
  }
  return (*tablet_result)->table_type();
}

void TabletPeer::SetFailed(const Status& error) {
  DCHECK(error_.get(std::memory_order_acquire) == nullptr);
  error_ = MakeAtomicUniquePtr<Status>(error);
  auto state = state_.load(std::memory_order_acquire);
  while (state != RaftGroupStatePB::FAILED && state != RaftGroupStatePB::QUIESCING &&
         state != RaftGroupStatePB::SHUTDOWN) {
    if (state_.compare_exchange_weak(state, RaftGroupStatePB::FAILED, std::memory_order_acq_rel)) {
      LOG_WITH_PREFIX(INFO) << "Changed state from " << RaftGroupStatePB_Name(state)
                            << " to FAILED";
      break;
    }
  }
}

Status TabletPeer::UpdateState(RaftGroupStatePB expected, RaftGroupStatePB new_state,
                               const std::string& error_message) {
  RaftGroupStatePB old = expected;
  if (!state_.compare_exchange_strong(old, new_state, std::memory_order_acq_rel)) {
    return STATUS_FORMAT(
        InvalidArgument, "$0 Expected state: $1, got: $2",
        error_message, RaftGroupStatePB_Name(expected), RaftGroupStatePB_Name(old));
  }

  LOG_WITH_PREFIX(INFO) << "Changed state from " << RaftGroupStatePB_Name(old) << " to "
                        << RaftGroupStatePB_Name(new_state);
  return Status::OK();
}

void TabletPeer::Enqueue(rpc::ThreadPoolTask* task) {
  rpc::ThreadPool* thread_pool = service_thread_pool_.load(std::memory_order_acquire);
  if (!thread_pool) {
    task->Done(STATUS(Aborted, "Thread pool not ready"));
    return;
  }

  thread_pool->Enqueue(task);
}

void TabletPeer::StrandEnqueue(rpc::StrandTask* task) {
  rpc::Strand* strand = strand_.get();
  if (!strand) {
    task->Done(STATUS(Aborted, "Thread pool not ready"));
    return;
  }

  strand->Enqueue(task);
}

bool TabletPeer::CanBeDeleted() {
  const auto consensus = GetRaftConsensusUnsafe();
  if (!consensus || consensus->LeaderTerm() == OpId::kUnknownTerm) {
    return false;
  }

  const auto tablet = shared_tablet_maybe_null();
  if (!tablet) {
    return false;
  }

  auto op_id = tablet->metadata()->GetOpIdToDeleteAfterAllApplied();
  if (!op_id.valid()) {
    return false;
  }

  const auto all_applied_op_id = consensus.get()->GetAllAppliedOpId();
  if (all_applied_op_id < op_id) {
    return false;
  }

  LOG_WITH_PREFIX(INFO) << Format(
      "Marked tablet $0 as requiring cleanup due to all replicas have been split (all applied op "
      "id: $1, split op id: $2, data state: $3)",
      tablet_id(), all_applied_op_id, op_id, TabletDataState_Name(data_state()));

  return true;
}

rpc::Scheduler& TabletPeer::scheduler() const {
  return messenger_->scheduler();
}

void TabletPeer::EnableFlushBootstrapState() {
  flush_bootstrap_state_enabled_.store(true, std::memory_order_relaxed);
}

bool TabletPeer::FlushBootstrapStateEnabled() const {
  return FLAGS_enable_flush_retryable_requests &&
      flush_bootstrap_state_enabled_.load(std::memory_order_relaxed);
}

Result<consensus::RetryableRequests> TabletPeer::GetRetryableRequests() {
  return VERIFY_RESULT(GetRaftConsensus())->GetRetryableRequests();
}

Status TabletPeer::FlushBootstrapState() {
  if (!FlushBootstrapStateEnabled()) {
    return STATUS(NotSupported, "flush_retryable_requests is not supported");
  }
  auto bootstrap_state_flusher = shared_bootstrap_state_flusher();
  SCHECK_FORMAT(bootstrap_state_flusher,
                IllegalState,
                "Tablet $0 bootstrap_state_flusher not initialized",
                tablet_id_);
  return bootstrap_state_flusher->FlushBootstrapState();
}

Result<OpId> TabletPeer::CopyBootstrapStateTo(const std::string& dest_path) {
  if (!FlushBootstrapStateEnabled()) {
    return STATUS(NotSupported, "flush_retryable_requests is not supported");
  }
  auto bootstrap_state_flusher = shared_bootstrap_state_flusher();
  SCHECK_FORMAT(bootstrap_state_flusher,
                IllegalState,
                "Tablet $0 bootstrap_state_flusher not initialized",
                tablet_id_);
  return bootstrap_state_flusher->CopyBootstrapStateTo(dest_path);
}

Status TabletPeer::CopyBootstrapStateForTabletSplit(const std::string& child_wal_dir) {
  if (!FlushBootstrapStateEnabled()) {
    return STATUS(NotSupported, "flush_retryable_requests is not supported");
  }
  // We do not use bootstrap_state_flusher for this variant. This means that we do not synchronize
  // with existing flushes, but tablet split prevents the flusher from proceeding anyways due to
  // the replica state lock being held (which is also why the flusher cannot be used here).
  return bootstrap_state_manager_->CopyTo(JoinPathSegments(
      child_wal_dir, tablet::TabletBootstrapStateManager::FileName()));
}

Status TabletPeer::SubmitFlushBootstrapStateTask() {
  if (!FlushBootstrapStateEnabled()) {
    return STATUS(NotSupported, "flush_retryable_requests is not supported");
  }
  auto bootstrap_state_flusher = shared_bootstrap_state_flusher();
  SCHECK_FORMAT(bootstrap_state_flusher,
                IllegalState,
                "Tablet $0 bootstrap_state_flusher not initialized",
                tablet_id_);
  return bootstrap_state_flusher->SubmitFlushBootstrapStateTask();
}

bool TabletPeer::TEST_HasBootstrapStateOnDisk() {
  if (!FlushBootstrapStateEnabled()) {
    return false;
  }
  auto bootstrap_state_flusher = shared_bootstrap_state_flusher();
  return bootstrap_state_flusher
      ? bootstrap_state_flusher->TEST_HasBootstrapStateOnDisk()
      : false;
}

TabletBootstrapFlushState TabletPeer::TEST_TabletBootstrapStateFlusherState() const {
  if (!FlushBootstrapStateEnabled()) {
    return TabletBootstrapFlushState::kFlushIdle;
  }
  auto bootstrap_state_flusher = shared_bootstrap_state_flusher();
  return bootstrap_state_flusher
      ? bootstrap_state_flusher->flush_state()
      : TabletBootstrapFlushState::kFlushIdle;
}

void TabletPeer::MinReplayTxnFirstWriteTimeUpdated(HybridTime first_write_ht) {
  if (first_write_ht && first_write_ht != HybridTime::kMax) {
    VLOG_WITH_PREFIX(2) << "min_replay_txn_first_write_ht updated: " << first_write_ht;
    bootstrap_state_manager_->bootstrap_state().SetMinReplayTxnFirstWriteTime(first_write_ht);
  }
}

Preparer* TabletPeer::DEBUG_GetPreparer() { return prepare_thread_.get(); }

bool TabletPeer::HasSufficientDiskSpaceForWrite() {
  if (log_ && !log_->HasSufficientDiskSpaceForWrite()) {
    return false;
  }
  return data_disk_space_checker_.HasSufficientDiskSpace();
}

void TabletPeer::NotifyCommitedAsyncWrites(const OpId& committed_op_id) {
  if (!committed_op_id) {
    return;
  }
  LOG_IF(DFATAL, committed_op_id < last_known_committed_op_id_.load(std::memory_order_acquire))
      << "Tablet " << tablet_id() << " committed op id: " << committed_op_id
      << ", last known committed op id: " << last_known_committed_op_id_;
  last_known_committed_op_id_.store(committed_op_id, std::memory_order_release);

  std::vector<std::pair<StdStatusCallback, Status>> callbacks;
  {
    std::lock_guard lock(async_write_queries_mutex_);
    auto it = in_flight_async_write_queries_.begin();
    while (it != in_flight_async_write_queries_.end()) {
      Status status;
      if (it->first.term != committed_op_id.term) {
        // Stale callback from previous term. Return NOT_THE_LEADER so the client can retry on
        // the new leader.
        status = STATUS_EC_FORMAT(
            IllegalState, tserver::TabletServerError(tserver::TabletServerErrorPB::NOT_THE_LEADER),
            "Unexpected tablet $0 term change. New term: $1, expected term: $2",
            tablet_id(), committed_op_id.term, it->first.term);
      } else if (it->first.index > committed_op_id.index) {
        break;
      }

      for (auto& callback : it->second) {
        callbacks.emplace_back(std::move(callback), std::move(status));
      }
      it = in_flight_async_write_queries_.erase(it);
    }
  }

  for (auto& [callback, status] : callbacks) {
    callback(status);
  }
}

void TabletPeer::FailAllAsyncWrites(const Status& status) {
  std::vector<StdStatusCallback> callbacks;
  {
    std::lock_guard lock(async_write_queries_mutex_);
    for (auto& [_, in_flight_callbacks] : in_flight_async_write_queries_) {
      MoveCollection(&in_flight_callbacks, &callbacks);
    }
    in_flight_async_write_queries_.clear();
  }

  for (auto& callback : callbacks) {
    callback(status);
  }
}

void TabletPeer::RegisterAsyncWrite(const OpId& op_id) {
  std::lock_guard lock(async_write_queries_mutex_);
  DCHECK(
      in_flight_async_write_queries_.empty() ||
      in_flight_async_write_queries_.back().first < op_id);
  in_flight_async_write_queries_.emplace_back(op_id, std::vector<StdStatusCallback>());
}

void TabletPeer::RegisterAsyncWriteCompletion(const OpId& op_id, StdStatusCallback&& callback) {
  // If the write is still in flight, add the callback to the list.
  {
    std::lock_guard lock(async_write_queries_mutex_);
    auto it = std::lower_bound(
        in_flight_async_write_queries_.begin(), in_flight_async_write_queries_.end(), op_id,
        [](const auto& pair, const OpId& op_id) { return pair.first < op_id; });
    if (it != in_flight_async_write_queries_.end() && it->first == op_id) {
      it->second.emplace_back(std::move(callback));
      return;
    }
  }

  callback(VerifyAsyncWriteCompletion(op_id));
}

Status TabletPeer::VerifyAsyncWriteReceived(
    const OpId& op_id, AllowLogLookup allow_log_lookup) {
  auto committed_op_id = last_known_committed_op_id_.load(std::memory_order_acquire);
  if (op_id.term == committed_op_id.term && op_id.index <= committed_op_id.index) {
    return Status::OK();
  }

  auto consensus = VERIFY_RESULT(GetRaftConsensus());
  auto [leader_state, first_index] = consensus->GetLeaderStateAndFirstIndexOfCurrentTerm();
  SCHECK_EC_FORMAT(
      leader_state.ok(), IllegalState,
      tserver::TabletServerError(tserver::TabletServerErrorPB::NOT_THE_LEADER),
      "Tablet $0: not a ready leader ($1), cannot verify async write $2 for read", tablet_id(),
      leader_state.status, op_id);

  if (op_id.term == leader_state.term) {
    // Same term - this peer appended the entry before acking the client.
    return Status::OK();
  }

  if (op_id.index < first_index) {
    if (op_id.term + 1 == leader_state.term) {
      // One term ago - the current term's NO_OP committed everything before it. Also covers
      // a split child on its first elected term, since first_index == split_op_id.index + 1.
      return Status::OK();
    }

    if (!allow_log_lookup) {
      return STATUS_EC_FORMAT(
          NotFound, TransactionError(TransactionErrorCode::kAborted),
          "Tablet $0: tablet leader moved more than once since async write $1 was issued "
          "(write from term $2, current term is $3). Retry the transaction.",
          tablet_id(), op_id, op_id.term, leader_state.term)
          .CloneAndAddErrorCode(
              tserver::TabletServerError(tserver::TabletServerErrorPB::ASYNC_WRITE_LOST));
    }

    // Two or more terms ago - an intermediate term may have overwritten the write, so validate it
    // from the log.
    auto log_op_id = consensus->LookupOpId(op_id.index);
    if (log_op_id.ok() && *log_op_id == op_id) {
      return Status::OK();
    }
    auto status = log_op_id.ok()
        ? STATUS_FORMAT(
              NotFound,
              "Tablet $0: async write $1 was overwritten by term $2 (found $3). Retry the "
              "transaction.",
              tablet_id(), op_id, log_op_id->term, *log_op_id)
        // E.g. the entry was GCed, so the write may have committed but can't be confirmed.
        : STATUS_FORMAT(
              NotFound,
              "Tablet $0: async write $1 could not be verified from the log: $2. Retry the "
              "transaction.",
              tablet_id(), op_id, log_op_id.status().message().ToBuffer());
    return status.CloneAndAddErrorCode(TransactionError(TransactionErrorCode::kAborted))
        .CloneAndAddErrorCode(
            tserver::TabletServerError(tserver::TabletServerErrorPB::ASYNC_WRITE_LOST));
  }

  // Write was lost/overwritten. Tag as a transaction abort so that the query layer can
  // transparently retry the transaction instead of surfacing an internal error.
  return STATUS_EC_FORMAT(
      NotFound, TransactionError(TransactionErrorCode::kAborted),
      "Tablet $0: tablet leader changed before async write $1 was replicated (first index of "
      "term $2 is $3). Retry the transaction.",
      tablet_id(), op_id, leader_state.term, first_index)
      .CloneAndAddErrorCode(
          tserver::TabletServerError(tserver::TabletServerErrorPB::ASYNC_WRITE_LOST));
}

Status TabletPeer::VerifyAsyncWriteCompletion(const OpId& op_id) {
  // Either the write completed on this leader and was already removed from the in-flight list, or
  // we're not the original leader. Either way, verify by checking if we have the requested op_id.
  // Replace NotFound with Aborted for the WaitForAsyncWriteRpc caller.
  auto status = VerifyAsyncWriteReceived(op_id);
  return status.IsNotFound() ? status.CloneAndReplaceCode(Status::kAborted) : status;
}

Status BackfillNamespaceIdIfNeeded(
    const TableId& table_id, tablet::RaftGroupMetadata& metadata, client::YBClient& client) {
  auto namespace_id = metadata.namespace_id();
  if (!namespace_id.empty()) {
    return Status::OK();
  }
  // If the namespace ID hasn't been backfilled yet, fetch it from master and populate the tablet
  // metadata.
  master::GetNamespaceInfoResponsePB resp;
  RETURN_NOT_OK(client.GetNamespaceInfoByTableId(table_id, &resp));
  namespace_id = resp.namespace_().id();
  SCHECK_FORMAT(
      !namespace_id.empty(), IllegalState, "Could not get namespace ID for table $0", table_id);
  return metadata.set_namespace_id(namespace_id);
}

bool TabletPeer::IsRunning() const {
  auto state = state_.load(std::memory_order_acquire);
  return state == RaftGroupStatePB::RUNNING;
}

}  // namespace yb::tablet
