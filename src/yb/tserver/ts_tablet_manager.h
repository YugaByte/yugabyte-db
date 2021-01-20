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
#ifndef YB_TSERVER_TS_TABLET_MANAGER_H
#define YB_TSERVER_TS_TABLET_MANAGER_H

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/container/static_vector.hpp>
#include <boost/optional/optional_fwd.hpp>
#include <boost/thread/shared_mutex.hpp>
#include <gtest/gtest_prod.h>

#include "yb/rocksdb/cache.h"
#include "yb/rocksdb/options.h"
#include "yb/client/async_initializer.h"
#include "yb/client/client_fwd.h"

#include "yb/common/constants.h"

#include "yb/consensus/consensus_fwd.h"
#include "yb/consensus/metadata.pb.h"

#include "yb/gutil/macros.h"
#include "yb/gutil/ref_counted.h"
#include "yb/rpc/rpc_fwd.h"
#include "yb/tablet/tablet_fwd.h"
#include "yb/tablet/tablet_options.h"
#include "yb/tablet/tablet_splitter.h"
#include "yb/tserver/tablet_peer_lookup.h"
#include "yb/tserver/tserver.pb.h"
#include "yb/tserver/tserver_admin.pb.h"
#include "yb/util/locks.h"
#include "yb/util/metrics.h"
#include "yb/util/rw_mutex.h"
#include "yb/util/shared_lock.h"
#include "yb/util/status.h"
#include "yb/util/threadpool.h"

namespace yb {

class GarbageCollector;
class PartitionSchema;
class FsManager;
class HostPort;
class Partition;
class Schema;
class BackgroundTask;

namespace consensus {
class RaftConfigPB;
} // namespace consensus

namespace master {
class ReportedTabletPB;
class TabletReportPB;
} // namespace master

namespace tserver {
class TabletServer;
class TsTabletManagerListener {
 public:
  virtual ~TsTabletManagerListener() {}
  virtual void StartedFlush(const TabletId& tablet_id) {}
};

using rocksdb::MemoryMonitor;

// Map of tablet id -> transition reason string.
typedef std::unordered_map<TabletId, std::string> TransitionInProgressMap;

class TransitionInProgressDeleter;
struct TabletCreationMetaData;
typedef boost::container::static_vector<TabletCreationMetaData, kNumSplitParts>
    SplitTabletsCreationMetaData;

// If 'expr' fails, log a message, tombstone the given tablet, and return the
// error status.
#define TOMBSTONE_NOT_OK(expr, meta, uuid, msg, ts_manager_ptr) \
  do { \
    Status _s = (expr); \
    if (PREDICT_FALSE(!_s.ok())) { \
      tserver::LogAndTombstone((meta), (msg), (uuid), _s, ts_manager_ptr); \
      return _s; \
    } \
  } while (0)

// Type of tablet directory.
YB_DEFINE_ENUM(TabletDirType, (kData)(kWal));

// Keeps track of the tablets hosted on the tablet server side.
//
// TODO: will also be responsible for keeping the local metadata about
// which tablets are hosted on this server persistent on disk, as well
// as re-opening all the tablets at startup, etc.
class TSTabletManager : public tserver::TabletPeerLookupIf, public tablet::TabletSplitter {
 public:
  typedef std::vector<std::shared_ptr<tablet::TabletPeer>> TabletPeers;
  typedef std::vector<tablet::TabletPtr> TabletPtrs;

  // Construct the tablet manager.
  // 'fs_manager' must remain valid until this object is destructed.
  TSTabletManager(FsManager* fs_manager,
                  TabletServer* server,
                  MetricRegistry* metric_registry);

  virtual ~TSTabletManager();

  // Load all tablet metadata blocks from disk, and open their respective tablets.
  // Upon return of this method all existing tablets are registered, but
  // the bootstrap is performed asynchronously.
  CHECKED_STATUS Init();
  CHECKED_STATUS Start();

  // Waits for all the bootstraps to complete.
  // Returns Status::OK if all tablets bootstrapped successfully. If
  // the bootstrap of any tablet failed returns the failure reason for
  // the first tablet whose bootstrap failed.
  CHECKED_STATUS WaitForAllBootstrapsToFinish();

  // Starts shutdown process.
  void StartShutdown();
  // Completes shutdown process and waits for it's completeness.
  void CompleteShutdown();

  ThreadPool* tablet_prepare_pool() const { return tablet_prepare_pool_.get(); }
  ThreadPool* raft_pool() const { return raft_pool_.get(); }
  ThreadPool* read_pool() const { return read_pool_.get(); }
  ThreadPool* append_pool() const { return append_pool_.get(); }

  // Create a new tablet and register it with the tablet manager. The new tablet
  // is persisted on disk and opened before this method returns.
  //
  // If tablet_peer is non-NULL, the newly created tablet will be returned.
  //
  // If another tablet already exists with this ID, logs a DFATAL
  // and returns a bad Status.
  CHECKED_STATUS CreateNewTablet(
      const string& table_id,
      const string& tablet_id,
      const Partition& partition,
      const string& namespace_name,
      const string& table_name,
      TableType table_type,
      const Schema& schema,
      const PartitionSchema& partition_schema,
      const boost::optional<IndexInfo>& index_info,
      consensus::RaftConfigPB config,
      std::shared_ptr<tablet::TabletPeer>* tablet_peer,
      const bool colocated = false);

  CHECKED_STATUS ApplyTabletSplit(tablet::SplitOperationState* state) override;

  // Delete the specified tablet.
  // 'delete_type' must be one of TABLET_DATA_DELETED or TABLET_DATA_TOMBSTONED
  // or else returns Status::IllegalArgument.
  // 'cas_config_opid_index_less_or_equal' is optionally specified to enable an
  // atomic DeleteTablet operation that only occurs if the latest committed
  // raft config change op has an opid_index equal to or less than the specified
  // value. If not, 'error_code' is set to CAS_FAILED and a non-OK Status is
  // returned.
  CHECKED_STATUS DeleteTablet(const TabletId& tablet_id,
                      tablet::TabletDataState delete_type,
                      const boost::optional<int64_t>& cas_config_opid_index_less_or_equal,
                      boost::optional<TabletServerErrorPB::Code>* error_code);

  // Lookup the given tablet peer by its ID.
  // Returns true if the tablet is found successfully.
  bool LookupTablet(const TabletId& tablet_id,
                    std::shared_ptr<tablet::TabletPeer>* tablet_peer) const;

  // Lookup the given tablet peer by its ID.
  // Returns NotFound error if the tablet is not found.
  Result<std::shared_ptr<tablet::TabletPeer>> LookupTablet(const TabletId& tablet_id) const;

  // Same as LookupTablet but doesn't acquired the shared lock.
  bool LookupTabletUnlocked(const TabletId& tablet_id,
                            std::shared_ptr<tablet::TabletPeer>* tablet_peer) const
      REQUIRES_SHARED(mutex_);

  CHECKED_STATUS GetTabletPeer(
      const TabletId& tablet_id,
      std::shared_ptr<tablet::TabletPeer>* tablet_peer) const override;

  const NodeInstancePB& NodeInstance() const override;

  CHECKED_STATUS GetRegistration(ServerRegistrationPB* reg) const override;

  // Initiate remote bootstrap of the specified tablet.
  // See the StartRemoteBootstrap() RPC declaration in consensus.proto for details.
  // Currently this runs the entire procedure synchronously.
  // TODO: KUDU-921: Run this procedure on a background thread.
  virtual CHECKED_STATUS
      StartRemoteBootstrap(const consensus::StartRemoteBootstrapRequestPB& req) override;

  // Generate a tablet report.
  //
  // This will report any tablets which have changed since the last acknowleged
  // tablet report. Once the report is successfully transferred, call
  // MarkTabletReportAcknowledged() to clear the incremental state. Otherwise, the
  // next tablet report will continue to include the same tablets until one
  // is acknowleged.
  // 'include_bootstrap' flag indicates whether to include bootstrapped tablets that have not
  // changed.  Normal reports include bootstrap information on every HB, but full reports do not.
  //
  // This is thread-safe to call along with tablet modification, but not safe
  // to call from multiple threads at the same time.
  void GenerateTabletReport(master::TabletReportPB* report, bool include_bootstrap = true);

  // Start a full tablet report and reset any incremental state tracking.
  void StartFullTabletReport(master::TabletReportPB* report);

  // Mark that the master successfully received and processed the given tablet report.
  // 'seq_num' - only remove tablets unchanged since the acknowledged report sequence number.
  // 'updates' - explicitly ACK'd updates from the Master, may be a subset of request tablets.
  // 'dirty_check' - DEBUG. Confirm we've processed all dirty tablets after a full sweep.
  void MarkTabletReportAcknowledged(int32_t seq_num,
                                    const master::TabletReportUpdatesPB& updates,
                                    bool dirty_check = false);

  // Adjust the max number of tablets that will be included in a single report.
  // This is normally controlled by a master-configured GFLAG.
  void SetReportLimit(int32_t limit) {
    std::lock_guard<RWMutex> write_lock(mutex_);
    report_limit_ = limit;
  }
  int32_t GetReportLimit() {
    SharedLock<RWMutex> read_lock(mutex_);
    return report_limit_;
  }

  // Get all of the tablets currently hosted on this server.
  void GetTabletPeers(TabletPeers* tablet_peers,
                      TabletPtrs* tablet_ptrs = nullptr) const;
  TabletPeers GetTabletPeers() const;
  void GetTabletPeersUnlocked(TabletPeers* tablet_peers) const REQUIRES_SHARED(mutex_);
  void PreserveLocalLeadersOnly(std::vector<const TabletId*>* tablet_ids) const;

  // Callback used for state changes outside of the control of TsTabletManager, such as a consensus
  // role change. They are applied asynchronously internally.
  void ApplyChange(const TabletId& tablet_id,
                   std::shared_ptr<consensus::StateChangeContext> context);

  // Marks tablet with 'tablet_id' dirty.
  // Used for state changes outside of the control of TsTabletManager, such as consensus role
  // changes.
  void MarkTabletDirty(const TabletId& tablet_id,
                       std::shared_ptr<consensus::StateChangeContext> context);

  void MarkTabletBeingRemoteBootstrapped(const TabletId& tablet_id, const TableId& table_id);

  void UnmarkTabletBeingRemoteBootstrapped(const TabletId& tablet_id, const TableId& table_id);

  // Returns the number of tablets in the "dirty" map, for use by unit tests.
  int GetNumDirtyTabletsForTests() const;

  // Return the number of tablets in RUNNING or BOOTSTRAPPING state.
  int GetNumLiveTablets() const;

  // Return the number of tablets for which this ts is a leader.
  int GetLeaderCount() const;

  // Set the number of tablets which are waiting to be bootstrapped and can go to RUNNING
  // state in the response proto. Also set the total number of runnable tablets on this tserver.
  // If the tablet manager itself is not initialized, then INT_MAX is set for both.
  CHECKED_STATUS GetNumTabletsPendingBootstrap(IsTabletServerReadyResponsePB* resp) const;

  CHECKED_STATUS RunAllLogGC();

  // Creates and updates the map of table to the set of tablets assigned per table per disk
  // for both data and wal directories.
  void GetAndRegisterDataAndWalDir(FsManager* fs_manager,
                                   const std::string& table_id,
                                   const TabletId& tablet_id,
                                   std::string* data_root_dir,
                                   std::string* wal_root_dir);
  // Updates the map of table to the set of tablets assigned per table per disk
  // for both of the given data and wal directories.
  void RegisterDataAndWalDir(FsManager* fs_manager,
                            const std::string& table_id,
                            const TabletId& tablet_id,
                            const std::string& data_root_dir,
                            const std::string& wal_root_dir);
  // Removes the tablet id assigned to the table and disk pair for both the data and WAL directory
  // as pointed by the data and wal directory map.
  void UnregisterDataWalDir(const std::string& table_id,
                            const TabletId& tablet_id,
                            const std::string& data_root_dir,
                            const std::string& wal_root_dir);

  bool IsTabletInTransition(const TabletId& tablet_id) const;

  TabletServer* server() { return server_; }

  MemoryMonitor* memory_monitor() { return tablet_options_.memory_monitor.get(); }

  // Flush some tablet if the memstore memory limit is exceeded
  void MaybeFlushTablet();

  client::YBClient& client();

  tablet::TabletOptions* TEST_tablet_options() { return &tablet_options_; }

  std::vector<std::shared_ptr<TsTabletManagerListener>> TEST_listeners;

 private:
  FRIEND_TEST(TsTabletManagerTest, TestPersistBlocks);
  FRIEND_TEST(TsTabletManagerTest, TestTombstonedTabletsAreUnregistered);

  // Flag specified when registering a TabletPeer.
  enum RegisterTabletPeerMode {
    NEW_PEER,
    REPLACEMENT_PEER
  };

  typedef std::unordered_set<TabletId> TabletIdUnorderedSet;

  // Maps directory to set of tablets (IDs) using that directory.
  typedef std::unordered_map<std::string, TabletIdUnorderedSet> TabletIdSetByDirectoryMap;

  // This is a map that takes a table id and maps it to a map of directory and
  // set of tablets using that directory.
  typedef std::unordered_map<TableId, TabletIdSetByDirectoryMap> TableDiskAssignmentMap;

  // Each tablet report is assigned a sequence number, so that subsequent
  // tablet reports only need to re-report those tablets which have
  // changed since the last report. Each tablet tracks the sequence
  // number at which it became dirty.
  struct TabletReportState {
    uint32_t change_seq;
  };
  typedef std::unordered_map<std::string, TabletReportState> DirtyMap;

  // Returns Status::OK() iff state_ == MANAGER_RUNNING.
  CHECKED_STATUS CheckRunningUnlocked(boost::optional<TabletServerErrorPB::Code>* error_code) const
      REQUIRES_SHARED(mutex_);

  // Registers the start of a tablet state transition by inserting the tablet
  // id and reason string into the transition_in_progress_ map.
  // 'reason' is a string included in the Status return when there is
  // contention indicating why the tablet is currently already transitioning.
  // Returns IllegalState if the tablet is already "locked" for a state
  // transition by some other operation.
  // On success, returns OK and populates 'deleter' with an object that removes
  // the map entry on destruction.
  CHECKED_STATUS StartTabletStateTransition(
      const TabletId& tablet_id, const std::string& reason,
      scoped_refptr<TransitionInProgressDeleter>* deleter);

  // Registers the start of a table state transition with "creating tablet" reason.
  // See StartTabletStateTransition.
  Result<scoped_refptr<TransitionInProgressDeleter>> StartTabletStateTransitionForCreation(
      const TabletId& tablet_id);

  // Open a tablet meta from the local file system by loading its superblock.
  CHECKED_STATUS OpenTabletMeta(const TabletId& tablet_id,
                        scoped_refptr<tablet::RaftGroupMetadata>* metadata);

  // Open a tablet whose metadata has already been loaded/created.
  // This method does not return anything as it can be run asynchronously.
  // Upon completion of this method the tablet should be initialized and running.
  // If something wrong happened on bootstrap/initialization the relevant error
  // will be set on TabletPeer along with the state set to FAILED.
  //
  // The tablet must be registered and an entry corresponding to this tablet
  // must be put into the transition_in_progress_ map before calling this
  // method. A TransitionInProgressDeleter must be passed as 'deleter' into
  // this method in order to remove that transition-in-progress entry when
  // opening the tablet is complete (in either a success or a failure case).
  void OpenTablet(const scoped_refptr<tablet::RaftGroupMetadata>& meta,
                  const scoped_refptr<TransitionInProgressDeleter>& deleter);

  // Open a tablet whose metadata has already been loaded.
  void BootstrapAndInitTablet(const scoped_refptr<tablet::RaftGroupMetadata>& meta,
                              std::shared_ptr<tablet::TabletPeer>* peer);

  // Add the tablet to the tablet map.
  // 'mode' specifies whether to expect an existing tablet to exist in the map.
  // If mode == NEW_PEER but a tablet with the same name is already registered,
  // or if mode == REPLACEMENT_PEER but a tablet with the same name is not
  // registered, a FATAL message is logged, causing a process crash.
  // Calls to this method are expected to be externally synchronized, typically
  // using the transition_in_progress_ map.
  CHECKED_STATUS RegisterTablet(const TabletId& tablet_id,
                                const std::shared_ptr<tablet::TabletPeer>& tablet_peer,
                                RegisterTabletPeerMode mode);

  // Create and register a new TabletPeer, given tablet metadata.
  // Calls RegisterTablet() with the given 'mode' parameter after constructing
  // the TablerPeer object. See RegisterTablet() for details about the
  // semantics of 'mode' and the locking requirements.
  Result<std::shared_ptr<tablet::TabletPeer>> CreateAndRegisterTabletPeer(
      const scoped_refptr<tablet::RaftGroupMetadata>& meta,
      RegisterTabletPeerMode mode);

  // Returns either table_data_assignment_map_ or table_wal_assignment_map_ depending on dir_type.
  TableDiskAssignmentMap* GetTableDiskAssignmentMapUnlocked(TabletDirType dir_type);

  // Returns assigned root dir of specified type for specified table and tablet.
  // If root dir is not registered for the specified table_id and tablet_id combination - returns
  // error.
  Result<const std::string&> GetAssignedRootDirForTablet(
      TabletDirType dir_type, const TableId& table_id, const TabletId& tablet_id);

  // Helper to generate the report for a single tablet.
  void CreateReportedTabletPB(const std::shared_ptr<tablet::TabletPeer>& tablet_peer,
                              master::ReportedTabletPB* reported_tablet);

  // Mark that the provided TabletPeer's state has changed. That should be taken into
  // account in the next report.
  //
  // NOTE: requires that the caller holds the lock.
  void MarkDirtyUnlocked(const TabletId& tablet_id,
                         std::shared_ptr<consensus::StateChangeContext> context) REQUIRES(mutex_);

  // Handle the case on startup where we find a tablet that is not in ready state. Generally, we
  // tombstone the replica.
  CHECKED_STATUS HandleNonReadyTabletOnStartup(
      const scoped_refptr<tablet::RaftGroupMetadata>& meta);

  CHECKED_STATUS StartSubtabletsSplit(
      const tablet::RaftGroupMetadata& source_tablet_meta, SplitTabletsCreationMetaData* tcmetas);

  // Creates tablet peer and schedules opening the tablet.
  // See CreateAndRegisterTabletPeer and OpenTablet.
  void CreatePeerAndOpenTablet(
      const tablet::RaftGroupMetadataPtr& meta,
      const scoped_refptr<TransitionInProgressDeleter>& deleter);

  // Return the tablet with oldest write still in its memstore
  std::shared_ptr<tablet::TabletPeer> TabletToFlush();

  TSTabletManagerStatePB state() const {
    SharedLock<RWMutex> lock(mutex_);
    return state_;
  }

  bool ClosingUnlocked() const REQUIRES_SHARED(mutex_);

  // Initializes the RaftPeerPB for the local peer.
  // Guaranteed to include both uuid and last_seen_addr fields.
  // Crashes with an invariant check if the RPC server is not currently in a
  // running state.
  void InitLocalRaftPeerPB();

  std::string LogPrefix() const;

  std::string TabletLogPrefix(const TabletId& tablet_id) const;

  void CleanupCheckpoints();

  void LogCacheGC(MemTracker* log_cache_mem_tracker, size_t required);

  // Check that the the global and per-table RBS limits are respected if flags
  // TEST_crash_if_remote_bootstrap_sessions_greater_than and
  // TEST_crash_if_remote_bootstrap_sessions_per_table_greater_than are non-zero.
  // Used only for tests.
  void MaybeDoChecksForTests(const TableId& table_id) REQUIRES_SHARED(mutex_);

  void CleanupSplitTablets();

  const CoarseTimePoint start_time_;

  FsManager* const fs_manager_;

  TabletServer* server_;

  consensus::RaftPeerPB local_peer_pb_;

  typedef std::unordered_map<TabletId, std::shared_ptr<tablet::TabletPeer>> TabletMap;

  // Lock protecting tablet_map_, dirty_tablets_, state_,
  // tablets_being_compacted_after_split_ and tablets_being_remote_bootstrapped_.
  mutable RWMutex mutex_;

  // Map from tablet ID to tablet
  TabletMap tablet_map_ GUARDED_BY(mutex_);

  // Map from table ID to count of children in data and wal directories.
  TableDiskAssignmentMap table_data_assignment_map_ GUARDED_BY(dir_assignment_mutex_);
  TableDiskAssignmentMap table_wal_assignment_map_ GUARDED_BY(dir_assignment_mutex_);
  mutable std::mutex dir_assignment_mutex_;

  // Map of tablet ids -> reason strings where the keys are tablets whose
  // bootstrap, creation, or deletion is in-progress
  TransitionInProgressMap transition_in_progress_ GUARDED_BY(transition_in_progress_mutex_);
  mutable std::mutex transition_in_progress_mutex_;

  // Tablets to include in the next tablet report. When a tablet is added/removed/added
  // locally and needs to be reported to the master, an entry is added to this map.
  // Tablets aren't removed from this Map until the Master acknowledges it in response.
  DirtyMap dirty_tablets_ GUARDED_BY(mutex_);

  typedef std::set<TabletId> TabletIdSet;

  TabletIdSet tablets_being_remote_bootstrapped_ GUARDED_BY(mutex_);

  TabletIdSet tablets_being_compacted_after_split_ GUARDED_BY(mutex_);

  // Used to keep track of the number of concurrent remote bootstrap sessions per table.
  std::unordered_map<TableId, TabletIdSet> tablets_being_remote_bootstrapped_per_table_;

  // Next tablet report seqno.
  uint32_t next_report_seq_ GUARDED_BY(mutex_) = 0;

  // Limit on the number of tablets to send in a single report.
  int32_t report_limit_ GUARDED_BY(mutex_) = std::numeric_limits<int32_t>::max();

  MetricRegistry* metric_registry_;

  TSTabletManagerStatePB state_ GUARDED_BY(mutex_);

  // Thread pool used to open the tablets async, whether bootstrap is required or not.
  std::unique_ptr<ThreadPool> open_tablet_pool_;

  // Thread pool for preparing transactions, shared between all tablets.
  std::unique_ptr<ThreadPool> tablet_prepare_pool_;

  // Thread pool for apply transactions, shared between all tablets.
  std::unique_ptr<ThreadPool> apply_pool_;

  // Thread pool for Raft-related operations, shared between all tablets.
  std::unique_ptr<ThreadPool> raft_pool_;

  // Thread pool for appender threads, shared between all tablets.
  std::unique_ptr<ThreadPool> append_pool_;

  // Thread pool for log allocation threads, shared between all tablets.
  std::unique_ptr<ThreadPool> allocation_pool_;

  // Thread pool for read ops, that are run in parallel, shared between all tablets.
  std::unique_ptr<ThreadPool> read_pool_;

  // Thread pool for manually triggering compactions for tablets created from a split.
  std::unique_ptr<ThreadPool> post_split_trigger_compaction_pool_;

  std::unique_ptr<rpc::Poller> tablets_cleaner_;

  // Used for scheduling flushes
  std::unique_ptr<BackgroundTask> background_task_;

  // For block cache and memory monitor shared across tablets
  tablet::TabletOptions tablet_options_;

  boost::optional<yb::client::AsyncClientInitialiser> async_client_init_;

  TabletPeers shutting_down_peers_;

  std::shared_ptr<GarbageCollector> block_based_table_gc_;
  std::shared_ptr<GarbageCollector> log_cache_gc_;

  std::shared_ptr<MemTracker> block_based_table_mem_tracker_;

  std::atomic<int32_t> num_tablets_being_remote_bootstrapped_{0};

  DISALLOW_COPY_AND_ASSIGN(TSTabletManager);
};

// Helper to delete the transition-in-progress entry from the corresponding set
// when tablet bootstrap, create, and delete operations complete.
class TransitionInProgressDeleter : public RefCountedThreadSafe<TransitionInProgressDeleter> {
 public:
  TransitionInProgressDeleter(TransitionInProgressMap* map, std::mutex* mutex,
                              const TabletId& tablet_id);

 private:
  friend class RefCountedThreadSafe<TransitionInProgressDeleter>;
  ~TransitionInProgressDeleter();

  TransitionInProgressMap* const in_progress_;
  std::mutex* const mutex_;
  const std::string tablet_id_;
};

// Print a log message using the given info and tombstone the specified tablet.
// If tombstoning the tablet fails, a FATAL error is logged, resulting in a crash.
// If ts_manager pointer is passed in, it will unregister from the directory assignment map.
void LogAndTombstone(const scoped_refptr<tablet::RaftGroupMetadata>& meta,
                     const std::string& msg,
                     const std::string& uuid,
                     const Status& s,
                     TSTabletManager* ts_manager = nullptr);

// Delete the tablet using the specified delete_type as the final metadata
// state. Deletes the on-disk data, as well as all WAL segments.
// If ts_manager pointer is passed in, it will unregister from the directory assignment map.
Status DeleteTabletData(const scoped_refptr<tablet::RaftGroupMetadata>& meta,
                        tablet::TabletDataState delete_type,
                        const std::string& uuid,
                        const yb::OpId& last_logged_opid,
                        TSTabletManager* ts_manager = nullptr);

// Return Status::IllegalState if leader_term < last_logged_term.
// Helper function for use with remote bootstrap.
Status CheckLeaderTermNotLower(const TabletId& tablet_id,
                               const std::string& uuid,
                               int64_t leader_term,
                               int64_t last_logged_term);

// Helper function to replace a stale tablet found from earlier failed tries.
Status HandleReplacingStaleTablet(scoped_refptr<tablet::RaftGroupMetadata> meta,
                                  std::shared_ptr<tablet::TabletPeer> old_tablet_peer,
                                  const TabletId& tablet_id,
                                  const std::string& uuid,
                                  const int64_t& leader_term);

CHECKED_STATUS ShutdownAndTombstoneTabletPeerNotOk(
    const Status& status, const tablet::TabletPeerPtr& tablet_peer,
    const tablet::RaftGroupMetadataPtr& meta, const std::string& uuid, const char* msg,
    TSTabletManager* ts_tablet_manager = nullptr);

} // namespace tserver
} // namespace yb
#endif /* YB_TSERVER_TS_TABLET_MANAGER_H */
