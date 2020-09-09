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
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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

#ifndef YB_TABLET_TABLET_PEER_H_
#define YB_TABLET_TABLET_PEER_H_

#include <atomic>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "yb/consensus/consensus_fwd.h"
#include "yb/consensus/consensus_context.h"
#include "yb/consensus/consensus_meta.h"
#include "yb/consensus/log.h"
#include "yb/gutil/callback.h"
#include "yb/gutil/ref_counted.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/thread_annotations.h"
#include "yb/rpc/rpc_fwd.h"

#include "yb/tablet/mvcc.h"
#include "yb/tablet/transaction_coordinator.h"
#include "yb/tablet/transaction_participant.h"
#include "yb/tablet/operation_order_verifier.h"
#include "yb/tablet/operations/operation_tracker.h"
#include "yb/tablet/operations/write_operation.h"
#include "yb/tablet/preparer.h"
#include "yb/tablet/tablet_options.h"
#include "yb/tablet/tablet_fwd.h"

#include "yb/util/metrics.h"
#include "yb/util/semaphore.h"

using yb::consensus::StateChangeContext;

namespace yb {

namespace log {
class LogAnchorRegistry;
}

namespace tserver {
class CatchUpServiceTest;
class UpdateTransactionResponsePB;
}

class MaintenanceManager;
class MaintenanceOp;
class ThreadPool;

namespace tablet {

struct TabletOnDiskSizeInfo {
  int64_t consensus_metadata_disk_size = 0;
  int64_t wal_files_disk_size = 0;
  int64_t sst_files_disk_size = 0;
  int64_t uncompressed_sst_files_disk_size = 0;
  int64_t sum_on_disk_size = 0;

  template <class PB>
  static TabletOnDiskSizeInfo FromPB(const PB& pb) {
    return {
      .consensus_metadata_disk_size = pb.consensus_metadata_disk_size(),
      .wal_files_disk_size = pb.wal_files_disk_size(),
      .sst_files_disk_size = pb.sst_files_disk_size(),
      .uncompressed_sst_files_disk_size = pb.uncompressed_sst_files_disk_size(),
      .sum_on_disk_size = pb.estimated_on_disk_size()
    };
  }

  template <class PB>
  void ToPB(PB* pb) const {
    pb->set_consensus_metadata_disk_size(consensus_metadata_disk_size);
    pb->set_wal_files_disk_size(wal_files_disk_size);
    pb->set_sst_files_disk_size(sst_files_disk_size);
    pb->set_uncompressed_sst_files_disk_size(uncompressed_sst_files_disk_size);
    pb->set_estimated_on_disk_size(sum_on_disk_size);
  }

  void operator+=(const TabletOnDiskSizeInfo& other) {
    consensus_metadata_disk_size += other.consensus_metadata_disk_size;
    wal_files_disk_size += other.wal_files_disk_size;
    sst_files_disk_size += other.sst_files_disk_size;
    uncompressed_sst_files_disk_size += other.uncompressed_sst_files_disk_size;
    sum_on_disk_size += other.sum_on_disk_size;
  }

  void RecomputeTotalSize() {
    sum_on_disk_size =
        consensus_metadata_disk_size +
        sst_files_disk_size +
        wal_files_disk_size;
  }
};

// A peer is a tablet consensus configuration, which coordinates writes to tablets.
// Each time Write() is called this class appends a new entry to a replicated
// state machine through a consensus algorithm, which makes sure that other
// peers see the same updates in the same order. In addition to this, this
// class also splits the work and coordinates multi-threaded execution.
class TabletPeer : public consensus::ConsensusContext,
                   public TransactionParticipantContext,
                   public TransactionCoordinatorContext,
                   public WriteOperationContext {
 public:
  typedef std::map<int64_t, int64_t> MaxIdxToSegmentSizeMap;

  // Creates TabletPeer.
  // `tablet_splitter` will be used for applying split tablet Raft operation.
  TabletPeer(
      const RaftGroupMetadataPtr& meta,
      const consensus::RaftPeerPB& local_peer_pb,
      const scoped_refptr<server::Clock>& clock,
      const std::string& permanent_uuid,
      Callback<void(std::shared_ptr<StateChangeContext> context)> mark_dirty_clbk,
      MetricRegistry* metric_registry,
      TabletSplitter* tablet_splitter,
      const std::shared_future<client::YBClient*>& client_future);

  ~TabletPeer();

  // Initializes the TabletPeer, namely creating the Log and initializing
  // Consensus.
  // split_op_id is the ID of split tablet Raft operation requesting split of this tablet or unset.
  CHECKED_STATUS InitTabletPeer(
      const TabletPtr& tablet,
      const std::shared_ptr<MemTracker>& server_mem_tracker,
      rpc::Messenger* messenger,
      rpc::ProxyCache* proxy_cache,
      const scoped_refptr<log::Log>& log,
      const scoped_refptr<MetricEntity>& metric_entity,
      ThreadPool* raft_pool,
      ThreadPool* tablet_prepare_pool,
      consensus::RetryableRequests* retryable_requests,
      const yb::OpId& split_op_id);

  // Starts the TabletPeer, making it available for Write()s. If this
  // TabletPeer is part of a consensus configuration this will connect it to other peers
  // in the consensus configuration.
  CHECKED_STATUS Start(const consensus::ConsensusBootstrapInfo& info);

  // Starts shutdown process.
  // Returns true if shutdown was just initiated, false if shutdown was already running.
  MUST_USE_RESULT bool StartShutdown();
  // Completes shutdown process and waits for it's completeness.
  void CompleteShutdown(IsDropTable is_drop_table = IsDropTable::kFalse);

  void Shutdown(IsDropTable is_drop_table = IsDropTable::kFalse);

  // Check that the tablet is in a RUNNING state.
  CHECKED_STATUS CheckRunning() const;

  // Check that the tablet is in a SHUTDOWN/NOT_STARTED state.
  CHECKED_STATUS CheckShutdownOrNotStarted() const;

  // Wait until the tablet is in a RUNNING state or if there's a timeout.
  // TODO have a way to wait for any state?
  CHECKED_STATUS WaitUntilConsensusRunning(const MonoDelta& timeout);

  // Submits a write to a tablet and executes it asynchronously.
  // The caller is expected to build and pass a WriteOperationState that points
  // to the RPC WriteRequest, WriteResponse, RpcContext and to the tablet's
  // MvccManager.
  // The operation_state is deallocated after use by this function.
  void WriteAsync(
      std::unique_ptr<WriteOperationState> operation_state, int64_t term, CoarseTimePoint deadline);

  void Submit(std::unique_ptr<Operation> operation, int64_t term) override;

  HybridTime Now() override;

  void UpdateClock(HybridTime hybrid_time) override;

  std::unique_ptr<UpdateTxnOperationState> CreateUpdateTransactionState(
      tserver::TransactionStatePB* request) override;

  void SubmitUpdateTransaction(
      std::unique_ptr<UpdateTxnOperationState> state, int64_t term) override;

  HybridTime SafeTimeForTransactionParticipant() override;

  void GetLastReplicatedData(RemoveIntentsData* data) override;

  void GetTabletStatusPB(TabletStatusPB* status_pb_out);

  // Used by consensus to create and start a new ReplicaOperation.
  CHECKED_STATUS StartReplicaOperation(
      const scoped_refptr<consensus::ConsensusRound>& round,
      HybridTime propagated_safe_time) override;

  // This is an override of a ConsensusContext method. This is called from
  // UpdateReplica -> EnqueuePreparesUnlocked on Raft heartbeats.
  void SetPropagatedSafeTime(HybridTime ht) override;

  // Returns false if it is preferable to don't apply write operation.
  bool ShouldApplyWrite() override;

  consensus::Consensus* consensus() const;
  consensus::RaftConsensus* raft_consensus() const;

  std::shared_ptr<consensus::Consensus> shared_consensus() const;

  Tablet* tablet() const {
    std::lock_guard<simple_spinlock> lock(lock_);
    return tablet_.get();
  }

  TabletPtr shared_tablet() const {
    std::lock_guard<simple_spinlock> lock(lock_);
    return tablet_;
  }

  const RaftGroupStatePB state() const {
    return state_.load(std::memory_order_acquire);
  }

  const TabletDataState data_state() const;

  // Returns the current Raft configuration.
  const consensus::RaftConfigPB RaftConfig() const;

  TabletStatusListener* status_listener() const {
    return status_listener_.get();
  }

  // Sets the tablet to a BOOTSTRAPPING state, indicating it is starting up.
  CHECKED_STATUS SetBootstrapping() {
    return UpdateState(RaftGroupStatePB::NOT_STARTED, RaftGroupStatePB::BOOTSTRAPPING, "");
  }

  CHECKED_STATUS UpdateState(RaftGroupStatePB expected, RaftGroupStatePB new_state,
                             const std::string& error_message);

  // sets the tablet state to FAILED additionally setting the error to the provided
  // one.
  void SetFailed(const Status& error);

  // Returns the error that occurred, when state is FAILED.
  CHECKED_STATUS error() const {
    Status *error;
    if ((error = error_.get(std::memory_order_acquire)) != nullptr) {
      // Once the error_ is set, we do not reset it to nullptr
      return *error;
    }
    return Status::OK();
  }

  // Returns a human-readable string indicating the state of the tablet.
  // Typically this looks like "NOT_STARTED", "TABLET_DATA_COPYING",
  // etc. For use in places like the Web UI.
  std::string HumanReadableState() const;

  // Adds list of transactions in-flight at the time of the call to
  // 'out'. OperationStatusPB objects are used to allow this method
  // to be used by both the web-UI and ts-cli.
  void GetInFlightOperations(Operation::TraceType trace_type,
                             std::vector<consensus::OperationStatusPB>* out) const;

  // Returns the minimum known log index that is in-memory or in-flight.
  // Used for selection of log segments to delete during Log GC.
  // If details is specified then this function appends explanation of how index was calculated
  // to it.
  Result<int64_t> GetEarliestNeededLogIndex(std::string* details = nullptr) const;

  // Returns a map of log index -> segment size, of all the segments that currently cannot be GCed
  // because in-memory structures have anchors in them.
  //
  // Returns a non-ok status if the tablet isn't running.
  CHECKED_STATUS GetMaxIndexesToSegmentSizeMap(MaxIdxToSegmentSizeMap* idx_size_map) const;

  // Returns the amount of bytes that would be GC'd if RunLogGC() was called.
  //
  // Returns a non-ok status if the tablet isn't running.
  CHECKED_STATUS GetGCableDataSize(int64_t* retention_size) const;

  // Returns true if it is safe to retrieve the log pointer using the log() function from this
  // tablet peer. Once the log pointer is initialized, it will stay valid for the lifetime of the
  // TabletPeer.
  bool log_available() const {
    return log_atomic_.load(std::memory_order_acquire) != nullptr;
  }

  // Return a pointer to the Log. TabletPeer keeps a reference to Log after Init(). This function
  // will crash if the log has not been initialized yet.
  log::Log* log() const;

  // Returns the OpId of the latest entry in the log, or a zero OpId if the log has not been
  // initialized.
  yb::OpId GetLatestLogEntryOpId() const;

  const server::ClockPtr& clock_ptr() const override {
    return clock_;
  }

  void Enqueue(rpc::ThreadPoolTask* task);
  void StrandEnqueue(rpc::StrandTask* task) override;

  const std::shared_future<client::YBClient*>& client_future() const override {
    return client_future_;
  }

  int64_t LeaderTerm() const override;
  consensus::LeaderStatus LeaderStatus(bool allow_stale = false) const;

  HybridTime HtLeaseExpiration() const override;

  const scoped_refptr<log::LogAnchorRegistry>& log_anchor_registry() const {
    return log_anchor_registry_;
  }

  // Returns the tablet_id of the tablet managed by this TabletPeer.
  // Returns the correct tablet_id even if the underlying tablet is not available
  // yet.
  const std::string& tablet_id() const override { return tablet_id_; }

  // Convenience method to return the permanent_uuid of this peer.
  const std::string& permanent_uuid() const override {
    return permanent_uuid_;
  }

  Result<OperationDriverPtr> NewOperationDriver(std::unique_ptr<Operation>* operation,
                                                int64_t term);

  Result<OperationDriverPtr> NewLeaderOperationDriver(
      std::unique_ptr<Operation>* operation, int64_t term);
  Result<OperationDriverPtr> NewReplicaOperationDriver(std::unique_ptr<Operation>* operation);

  // Tells the tablet's log to garbage collect.
  CHECKED_STATUS RunLogGC();

  // Register the maintenance ops associated with this peer's tablet, also invokes
  // Tablet::RegisterMaintenanceOps().
  void RegisterMaintenanceOps(MaintenanceManager* maintenance_manager);

  // Unregister the maintenance ops associated with this peer's tablet.
  // This method is not thread safe.
  void UnregisterMaintenanceOps();

  // Return pointer to the transaction tracker for this peer.
  const OperationTracker* operation_tracker() const { return &operation_tracker_; }

  const RaftGroupMetadataPtr& tablet_metadata() const {
    return meta_;
  }

  CHECKED_STATUS set_cdc_min_replicated_index(int64_t cdc_min_replicated_index);

  CHECKED_STATUS set_cdc_min_replicated_index_unlocked(int64_t cdc_min_replicated_index);

  CHECKED_STATUS reset_cdc_min_replicated_index_if_stale();

  TableType table_type();

  // Return granular types of on-disk size of this tablet replica, in bytes.
  TabletOnDiskSizeInfo GetOnDiskSizeInfo() const REQUIRES(lock_);

  // Returns the number of segments in log_.
  int GetNumLogSegments() const;

  std::string LogPrefix() const;

 protected:
  friend class RefCountedThreadSafe<TabletPeer>;
  friend class TabletPeerTest;
  FRIEND_TEST(TabletPeerTest, TestDMSAnchorPreventsLogGC);
  FRIEND_TEST(TabletPeerTest, TestActiveOperationPreventsLogGC);

  // Wait until the TabletPeer is fully in SHUTDOWN state.
  void WaitUntilShutdown();

  // After bootstrap is complete and consensus is setup this initiates the transactions
  // that were not complete on bootstrap.
  // Not implemented yet. See .cc file.
  CHECKED_STATUS StartPendingOperations(consensus::RaftPeerPB::Role my_role,
                                        const consensus::ConsensusBootstrapInfo& bootstrap_info);

  scoped_refptr<OperationDriver> CreateOperationDriver();

  virtual std::unique_ptr<Operation> CreateOperation(consensus::ReplicateMsg* replicate_msg);

  const RaftGroupMetadataPtr meta_;

  const std::string tablet_id_;

  const consensus::RaftPeerPB local_peer_pb_;

  // The atomics state_, error_ and has_consensus_ maintain information about the tablet peer.
  // While modifying the other fields in tablet peer, state_ is modified last.
  // error_ is set before state_ is set to an error state.
  std::atomic<enum RaftGroupStatePB> state_;
  AtomicUniquePtr<Status> error_;
  std::atomic<bool> has_consensus_ = {false};

  OperationTracker operation_tracker_;
  OperationOrderVerifier operation_order_verifier_;

  scoped_refptr<log::Log> log_;
  std::atomic<log::Log*> log_atomic_{nullptr};

  TabletPtr tablet_;
  rpc::ProxyCache* proxy_cache_;
  std::shared_ptr<consensus::RaftConsensus> consensus_;
  gscoped_ptr<TabletStatusListener> status_listener_;
  simple_spinlock prepare_replicate_lock_;

  // Lock protecting state_ as well as smart pointers to collaborating
  // classes such as tablet_ and consensus_.
  mutable simple_spinlock lock_;

  // Lock taken during Init/Shutdown which ensures that only a single thread
  // attempts to perform major lifecycle operations (Init/Shutdown) at once.
  // This must be acquired before acquiring lock_ if they are acquired together.
  // We don't just use lock_ since the lifecycle operations may take a while
  // and we'd like other threads to be able to quickly poll the state_ variable
  // during them in order to reject RPCs, etc.
  mutable simple_spinlock state_change_lock_;

  std::unique_ptr<Preparer> prepare_thread_;

  scoped_refptr<server::Clock> clock_;

  scoped_refptr<log::LogAnchorRegistry> log_anchor_registry_;

  // Function to mark this TabletPeer's tablet as dirty in the TSTabletManager.
  // This function must be called any time the cluster membership or cluster
  // leadership changes. Note that this function is called synchronously on the followers
  // or leader via the consensus round completion callback of NonTxRoundReplicationFinished.
  // Hence this should be a relatively lightweight function - e.g., update in-memory only state
  // and defer any other heavy duty operations to a thread pool.
  Callback<void(std::shared_ptr<consensus::StateChangeContext> context)> mark_dirty_clbk_;

  // List of maintenance operations for the tablet that need information that only the peer
  // can provide.
  std::vector<MaintenanceOp*> maintenance_ops_;

  // Cache the permanent of the tablet UUID to retrieve it without a lock in the common case.
  const std::string permanent_uuid_;

  std::atomic<rpc::ThreadPool*> service_thread_pool_{nullptr};
  AtomicUniquePtr<rpc::Strand> strand_;

  OperationCounter preparing_operations_counter_;

  // Serializes access to set_cdc_min_replicated_index and reset_cdc_min_replicated_index_if_stale
  // and protects cdc_min_replicated_index_refresh_time_ for reads and writes.
  mutable simple_spinlock cdc_min_replicated_index_lock_;
  MonoTime cdc_min_replicated_index_refresh_time_ = MonoTime::Min();

 private:
  HybridTime ReportReadRestart() override;

  FixedHybridTimeLease HybridTimeLease(MicrosTime min_allowed, CoarseTimePoint deadline);
  HybridTime PreparePeerRequest() override;
  void MajorityReplicated() override;
  void ChangeConfigReplicated(const consensus::RaftConfigPB& config) override;
  uint64_t NumSSTFiles() override;
  void ListenNumSSTFilesChanged(std::function<void()> listener) override;

  MetricRegistry* metric_registry_;

  bool IsLeader() override {
    return LeaderTerm() != OpId::kUnknownTerm;
  }

  TabletSplitter* tablet_splitter_;

  std::shared_future<client::YBClient*> client_future_;

  DISALLOW_COPY_AND_ASSIGN(TabletPeer);
};

}  // namespace tablet
}  // namespace yb

#endif /* YB_TABLET_TABLET_PEER_H_ */
