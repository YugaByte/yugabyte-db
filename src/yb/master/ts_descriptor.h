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
#ifndef YB_MASTER_TS_DESCRIPTOR_H
#define YB_MASTER_TS_DESCRIPTOR_H

#include <shared_mutex>

#include <memory>
#include <mutex>
#include <string>

#include "yb/gutil/gscoped_ptr.h"

#include "yb/master/master_fwd.h"
#include "yb/master/master.pb.h"

#include "yb/tserver/tserver_service.proxy.h"

#include "yb/util/capabilities.h"
#include "yb/util/locks.h"
#include "yb/util/monotime.h"
#include "yb/util/status.h"
#include "yb/util/shared_ptr_tuple.h"
#include "yb/util/shared_lock.h"

namespace yb {

class NodeInstancePB;

namespace consensus {
class ConsensusServiceProxy;
}

namespace tserver {
class TabletServerAdminServiceProxy;
class TabletServerServiceProxy;
}

namespace master {

class TSRegistrationPB;
class TSInformationPB;
class ReplicationInfoPB;
class TServerMetricsPB;

typedef util::SharedPtrTuple<tserver::TabletServerAdminServiceProxy,
                             tserver::TabletServerServiceProxy,
                             consensus::ConsensusServiceProxy> ProxyTuple;

// Master-side view of a single tablet server.
//
// Tracks the last heartbeat, status, instance identifier, etc.
// This class is thread-safe.
class TSDescriptor {
 public:
  static Result<TSDescriptorPtr> RegisterNew(
      const NodeInstancePB& instance,
      const TSRegistrationPB& registration,
      CloudInfoPB local_cloud_info,
      rpc::ProxyCache* proxy_cache,
      RegisteredThroughHeartbeat registered_through_heartbeat = RegisteredThroughHeartbeat::kTrue);

  static std::string generate_placement_id(const CloudInfoPB& ci);

  virtual ~TSDescriptor();

  // Set the last-heartbeat time to now.
  void UpdateHeartbeatTime();

  // Return the amount of time since the last heartbeat received
  // from this TS.
  MonoDelta TimeSinceHeartbeat() const;

  // Register this tablet server.
  CHECKED_STATUS Register(const NodeInstancePB& instance,
                          const TSRegistrationPB& registration,
                          CloudInfoPB local_cloud_info,
                          rpc::ProxyCache* proxy_cache);

  const std::string &permanent_uuid() const { return permanent_uuid_; }
  int64_t latest_seqno() const;

  bool has_tablet_report() const;
  void set_has_tablet_report(bool has_report);

  bool registered_through_heartbeat() const;

  // Returns TSRegistrationPB for this TSDescriptor.
  TSRegistrationPB GetRegistration() const;

  // Returns TSInformationPB for this TSDescriptor.
  const std::shared_ptr<TSInformationPB> GetTSInformationPB() const;

  // Helper function to tell if this TS matches the cloud information provided. For now, we have
  // no wildcard functionality, so it will have to explicitly match each individual component.
  // Later, this might be extended to say if this TS is part of some wildcard expression for cloud
  // information (eg: aws.us-west.* will match any TS in aws.us-west.1a or aws.us-west.1b, etc.).
  bool MatchesCloudInfo(const CloudInfoPB& cloud_info) const;

  // Return the pre-computed placement_id, comprised of the cloud_info data.
  std::string placement_id() const;

  std::string placement_uuid() const;

  bool IsRunningOn(const HostPortPB& hp) const;

  // Should this ts have any leader load on it.
  virtual bool IsAcceptingLeaderLoad(const ReplicationInfoPB& replication_info) const;

  // Return an RPC proxy to a service.
  template <class TProxy>
  CHECKED_STATUS GetProxy(std::shared_ptr<TProxy>* proxy) {
    return GetOrCreateProxy(proxy, &proxies_.get<TProxy>());
  }

  // Increment the accounting of the number of replicas recently created on this
  // server. This value will automatically decay over time.
  void IncrementRecentReplicaCreations();

  // Return the number of replicas which have recently been created on this
  // TS. This number is incremented when replicas are placed on the TS, and
  // then decayed over time. This method is not 'const' because each call
  // actually performs the time-based decay.
  double RecentReplicaCreations();

  // Set the number of live replicas (i.e. running or bootstrapping).
  void set_num_live_replicas(int num_live_replicas) {
    DCHECK_GE(num_live_replicas, 0);
    std::lock_guard<decltype(lock_)> l(lock_);
    num_live_replicas_ = num_live_replicas;
  }

  // Return the number of live replicas (i.e running or bootstrapping).
  int num_live_replicas() const {
    SharedLock<decltype(lock_)> l(lock_);
    return num_live_replicas_;
  }

  void set_leader_count(int leader_count) {
    DCHECK_GE(leader_count, 0);
    std::lock_guard<decltype(lock_)> l(lock_);
    leader_count_ = leader_count;
  }

  int leader_count() const {
    SharedLock<decltype(lock_)> l(lock_);
    return leader_count_;
  }

  void set_total_memory_usage(uint64_t total_memory_usage) {
    std::lock_guard<decltype(lock_)> l(lock_);
    ts_metrics_.total_memory_usage = total_memory_usage;
  }

  uint64_t total_memory_usage() {
    SharedLock<decltype(lock_)> l(lock_);
    return ts_metrics_.total_memory_usage;
  }

  void set_total_sst_file_size (uint64_t total_sst_file_size) {
    std::lock_guard<decltype(lock_)> l(lock_);
    ts_metrics_.total_sst_file_size = total_sst_file_size;
  }

  void set_uncompressed_sst_file_size (uint64_t uncompressed_sst_file_size) {
    std::lock_guard<decltype(lock_)> l(lock_);
    ts_metrics_.uncompressed_sst_file_size = uncompressed_sst_file_size;
  }

  void set_num_sst_files (uint64_t num_sst_files) {
    std::lock_guard<decltype(lock_)> l(lock_);
    ts_metrics_.num_sst_files = num_sst_files;
  }

  uint64_t total_sst_file_size() {
    SharedLock<decltype(lock_)> l(lock_);
    return ts_metrics_.total_sst_file_size;
  }

  uint64_t uncompressed_sst_file_size() {
    SharedLock<decltype(lock_)> l(lock_);
    return ts_metrics_.uncompressed_sst_file_size;
  }

  uint64_t num_sst_files() {
    SharedLock<decltype(lock_)> l(lock_);
    return ts_metrics_.num_sst_files;
  }

  void set_read_ops_per_sec(double read_ops_per_sec) {
    std::lock_guard<decltype(lock_)> l(lock_);
    ts_metrics_.read_ops_per_sec = read_ops_per_sec;
  }

  double read_ops_per_sec() {
    SharedLock<decltype(lock_)> l(lock_);
    return ts_metrics_.read_ops_per_sec;
  }

  void set_write_ops_per_sec(double write_ops_per_sec) {
    std::lock_guard<decltype(lock_)> l(lock_);
    ts_metrics_.write_ops_per_sec = write_ops_per_sec;
  }

  double write_ops_per_sec() {
    SharedLock<decltype(lock_)> l(lock_);
    return ts_metrics_.write_ops_per_sec;
  }

  uint64_t uptime_seconds() {
    SharedLock<decltype(lock_)> l(lock_);
    return ts_metrics_.uptime_seconds;
  }

  void UpdateMetrics(const TServerMetricsPB& metrics);

  void GetMetrics(TServerMetricsPB* metrics);

  void ClearMetrics() {
    std::lock_guard<decltype(lock_)> l(lock_);
    ts_metrics_.ClearMetrics();
  }

  // Set of methods to keep track of pending tablet deletes for a tablet server. We use them to
  // avoid assigning more tablets to a tserver that might be potentially unresponsive.
  bool HasTabletDeletePending() const;
  bool IsTabletDeletePending(const std::string& tablet_id) const;
  void AddPendingTabletDelete(const std::string& tablet_id);
  void ClearPendingTabletDelete(const std::string& tablet_id);
  std::string PendingTabletDeleteToString() const;

  std::string ToString() const;

  // Indicates that this descriptor was removed from the cluster and shouldn't be surfaced.
  bool IsRemoved() const {
    return removed_;
  }

  void SetRemoved() {
    removed_ = true;
  }

  explicit TSDescriptor(std::string perm_id);

  std::size_t NumTasks() const;

 protected:
  virtual CHECKED_STATUS RegisterUnlocked(const NodeInstancePB& instance,
                                          const TSRegistrationPB& registration,
                                          CloudInfoPB local_cloud_info,
                                          rpc::ProxyCache* proxy_cache);

  mutable rw_spinlock lock_;
 private:
  template <class TProxy>
  CHECKED_STATUS GetOrCreateProxy(std::shared_ptr<TProxy>* result,
                                  std::shared_ptr<TProxy>* result_cache);

  FRIEND_TEST(TestTSDescriptor, TestReplicaCreationsDecay);
  template<class ClusterLoadBalancerClass> friend class TestLoadBalancerBase;

  // Uses DNS to resolve registered hosts to a single endpoint.
  Result<HostPort> GetHostPortUnlocked() const;

  void DecayRecentReplicaCreationsUnlocked();

  struct TSMetrics {

    // Stores the total RAM usage of a tserver that is sent in every heartbeat.
    uint64_t total_memory_usage = 0;

    // Stores the total size of all the sst files in a tserver
    uint64_t total_sst_file_size = 0;
    uint64_t uncompressed_sst_file_size = 0;
    uint64_t num_sst_files = 0;

    double read_ops_per_sec = 0;

    double write_ops_per_sec = 0;

    uint64_t uptime_seconds = 0;

    void ClearMetrics() {
      total_memory_usage = 0;
      total_sst_file_size = 0;
      uncompressed_sst_file_size = 0;
      num_sst_files = 0;
      read_ops_per_sec = 0;
      write_ops_per_sec = 0;
      uptime_seconds = 0;
    }
  };

  struct TSMetrics ts_metrics_;

  const std::string permanent_uuid_;
  CloudInfoPB local_cloud_info_;
  rpc::ProxyCache* proxy_cache_;
  int64_t latest_seqno_;

  // The last time a heartbeat was received for this node.
  MonoTime last_heartbeat_;

  // Set to true once this instance has reported all of its tablets.
  bool has_tablet_report_;

  // The number of times this tablet server has recently been selected to create a
  // tablet replica. This value decays back to 0 over time.
  double recent_replica_creations_;
  MonoTime last_replica_creations_decay_;

  // The number of live replicas on this host, from the last heartbeat.
  int num_live_replicas_;

  // The number of tablets for which this ts is a leader.
  int leader_count_;

  std::shared_ptr<TSInformationPB> ts_information_;
  std::string placement_id_;

  // The (read replica) cluster uuid to which this tserver belongs.
  std::string placement_uuid_;

  enterprise::ProxyTuple proxies_;

  // Set of tablet uuids for which a delete is pending on this tablet server.
  std::set<std::string> tablets_pending_delete_;

  // Capabilities of this tablet server.
  google::protobuf::RepeatedField<CapabilityId> capabilities_;

  // We don't remove TSDescriptor's from the master's in memory map since several classes hold
  // references to this object and those would be invalidated if we remove the descriptor from
  // the master's map. As a result, we just store a boolean indicating this entry is removed and
  // shouldn't be surfaced.
  bool removed_ = false;

  // Did this tserver register by heartbeating through master. If false, we registered through
  // peer's Raft config.
  RegisteredThroughHeartbeat registered_through_heartbeat_ = RegisteredThroughHeartbeat::kTrue;

  DISALLOW_COPY_AND_ASSIGN(TSDescriptor);
};

template <class TProxy>
Status TSDescriptor::GetOrCreateProxy(std::shared_ptr<TProxy>* result,
                                      std::shared_ptr<TProxy>* result_cache) {
  {
    std::lock_guard<decltype(lock_)> l(lock_);
    if (*result_cache) {
      *result = *result_cache;
      return Status::OK();
    }
    auto hostport = VERIFY_RESULT(GetHostPortUnlocked());
    if (!(*result_cache)) {
      *result_cache = std::make_shared<TProxy>(proxy_cache_, hostport);
    }
    *result = *result_cache;
  }
  return Status::OK();
}

} // namespace master
} // namespace yb
#endif // YB_MASTER_TS_DESCRIPTOR_H
