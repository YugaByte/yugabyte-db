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

#include "yb/client/meta_cache.h"

#include <shared_mutex>
#include <mutex>

#include <glog/logging.h>

#include "yb/client/client.h"
#include "yb/client/client_error.h"
#include "yb/client/client-internal.h"
#include "yb/client/table.h"

#include "yb/common/schema.h"
#include "yb/common/wire_protocol.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/master/master.pb.h"
#include "yb/master/master.proxy.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/rpc.h"

#include "yb/tserver/local_tablet_server.h"
#include "yb/tserver/tserver_service.proxy.h"

#include "yb/util/algorithm_util.h"
#include "yb/util/flag_tags.h"
#include "yb/util/net/dns_resolver.h"
#include "yb/util/net/net_util.h"
#include "yb/util/scope_exit.h"
#include "yb/util/shared_lock.h"

using std::string;
using std::map;
using std::shared_ptr;
using strings::Substitute;
using namespace std::literals;  // NOLINT

DEFINE_int32(max_concurrent_master_lookups, 500,
             "Maximum number of concurrent tablet location lookups from YB client to master");

DEFINE_test_flag(bool, verify_all_replicas_alive, false,
                 "If set, when a RemoteTablet object is destroyed, we will verify that all its "
                 "replicas are not marked as failed");

DEFINE_int32(retry_failed_replica_ms, 60 * 1000,
             "Time in milliseconds to wait for before retrying a failed replica");

DEFINE_int64(meta_cache_lookup_throttling_step_ms, 5,
             "Step to increment delay between calls during lookup throttling.");

DEFINE_int64(meta_cache_lookup_throttling_max_delay_ms, 1000,
             "Max delay between calls during lookup throttling.");

METRIC_DEFINE_histogram(
  server, dns_resolve_latency_during_init_proxy,
  "yb.client.MetaCache.InitProxy DNS Resolve",
  yb::MetricUnit::kMicroseconds,
  "Microseconds spent resolving DNS requests during MetaCache::InitProxy",
  60000000LU, 2);

namespace yb {

using consensus::RaftPeerPB;
using master::GetTableLocationsRequestPB;
using master::GetTableLocationsResponsePB;
using master::MasterServiceProxy;
using master::TabletLocationsPB;
using master::TabletLocationsPB_ReplicaPB;
using master::TSInfoPB;
using rpc::Messenger;
using rpc::Rpc;
using tablet::RaftGroupStatePB;
using tserver::LocalTabletServer;
using tserver::TabletServerServiceProxy;

namespace client {

namespace internal {

namespace {

// We join tablet partitions to groups, so tablet state info in one group requested with single
// RPC call to master.
// kPartitionGroupSize defines size of this group.
#ifdef NDEBUG
const size_t kPartitionGroupSize = 64;
#else
const size_t kPartitionGroupSize = 4;
#endif

std::atomic<int64_t> lookup_serial_{1};

} // namespace

////////////////////////////////////////////////////////////

RemoteTabletServer::RemoteTabletServer(const master::TSInfoPB& pb)
    : uuid_(pb.permanent_uuid()) {
  Update(pb);
}

RemoteTabletServer::RemoteTabletServer(const string& uuid,
                                       const shared_ptr<TabletServerServiceProxy>& proxy,
                                       const LocalTabletServer* local_tserver)
    : uuid_(uuid),
      proxy_(proxy),
      local_tserver_(local_tserver) {
  LOG_IF(DFATAL, proxy && !IsLocal()) << "Local tserver has non-local proxy";
}

Status RemoteTabletServer::InitProxy(YBClient* client) {
  {
    SharedLock<rw_spinlock> lock(mutex_);

    if (proxy_) {
      // Already have a proxy created.
      return Status::OK();
    }
  }

  std::lock_guard<rw_spinlock> lock(mutex_);

  if (proxy_) {
    // Already have a proxy created.
    return Status::OK();
  }

  if (!dns_resolve_histogram_) {
    auto metric_entity = client->metric_entity();
    if (metric_entity) {
      dns_resolve_histogram_ = METRIC_dns_resolve_latency_during_init_proxy.Instantiate(
          metric_entity);
    }
  }

  // TODO: if the TS advertises multiple host/ports, pick the right one
  // based on some kind of policy. For now just use the first always.
  auto hostport = HostPortFromPB(DesiredHostPort(
      public_rpc_hostports_, private_rpc_hostports_, cloud_info_pb_,
      client->data_->cloud_info_pb_));
  CHECK(!hostport.host().empty());
  ScopedDnsTracker dns_tracker(dns_resolve_histogram_.get());
  proxy_.reset(new TabletServerServiceProxy(client->data_->proxy_cache_.get(), hostport));
  proxy_endpoint_ = hostport;

  return Status::OK();
}

void RemoteTabletServer::Update(const master::TSInfoPB& pb) {
  CHECK_EQ(pb.permanent_uuid(), uuid_);

  std::lock_guard<rw_spinlock> lock(mutex_);
  private_rpc_hostports_ = pb.private_rpc_addresses();
  public_rpc_hostports_ = pb.broadcast_addresses();
  cloud_info_pb_ = pb.cloud_info();
  capabilities_.assign(pb.capabilities().begin(), pb.capabilities().end());
  std::sort(capabilities_.begin(), capabilities_.end());
}

bool RemoteTabletServer::IsLocal() const {
  return local_tserver_ != nullptr;
}

const std::string& RemoteTabletServer::permanent_uuid() const {
  return uuid_;
}

const CloudInfoPB& RemoteTabletServer::cloud_info() const {
  return cloud_info_pb_;
}

const google::protobuf::RepeatedPtrField<HostPortPB>&
    RemoteTabletServer::public_rpc_hostports() const {
  return public_rpc_hostports_;
}

const google::protobuf::RepeatedPtrField<HostPortPB>&
    RemoteTabletServer::private_rpc_hostports() const {
  return private_rpc_hostports_;
}

shared_ptr<TabletServerServiceProxy> RemoteTabletServer::proxy() const {
  SharedLock<rw_spinlock> lock(mutex_);
  return proxy_;
}

::yb::HostPort RemoteTabletServer::ProxyEndpoint() const {
  std::shared_lock<rw_spinlock> lock(mutex_);
  return proxy_endpoint_;
}

string RemoteTabletServer::ToString() const {
  string ret = "{ uuid: " + uuid_;
  SharedLock<rw_spinlock> lock(mutex_);
  if (!private_rpc_hostports_.empty()) {
    ret += Format(" private: $0", private_rpc_hostports_);
  }
  if (!public_rpc_hostports_.empty()) {
    ret += Format(" public: $0", public_rpc_hostports_);
  }
  ret += Format(" cloud_info: $0", cloud_info_pb_);
  return ret;
}

bool RemoteTabletServer::HasHostFrom(const std::unordered_set<std::string>& hosts) const {
  SharedLock<rw_spinlock> lock(mutex_);
  for (const auto& hp : private_rpc_hostports_) {
    if (hosts.count(hp.host())) {
      return true;
    }
  }
  for (const auto& hp : public_rpc_hostports_) {
    if (hosts.count(hp.host())) {
      return true;
    }
  }
  return false;
}

bool RemoteTabletServer::HasCapability(CapabilityId capability) const {
  SharedLock<rw_spinlock> lock(mutex_);
  return std::binary_search(capabilities_.begin(), capabilities_.end(), capability);
}

////////////////////////////////////////////////////////////

RemoteTablet::~RemoteTablet() {
  if (PREDICT_FALSE(FLAGS_TEST_verify_all_replicas_alive)) {
    // Let's verify that none of the replicas are marked as failed. The test should always wait
    // enough time so that the lookup cache can be refreshed after force_lookup_cache_refresh_secs.
    for (const auto& replica : replicas_) {
      if (replica.Failed()) {
        LOG_WITH_PREFIX(FATAL) << "Remote tablet server " << replica.ts->ToString()
                               << " with role " << consensus::RaftPeerPB::Role_Name(replica.role)
                               << " is marked as failed";
      }
    }
  }
}

void RemoteTablet::Refresh(
    const TabletServerMap& tservers,
    const google::protobuf::RepeatedPtrField<TabletLocationsPB_ReplicaPB>& replicas) {
  // Adopt the data from the successful response.
  std::lock_guard<rw_spinlock> lock(mutex_);
  std::vector<std::string> old_uuids;
  old_uuids.reserve(replicas_.size());
  for (const auto& replica : replicas_) {
    old_uuids.push_back(replica.ts->permanent_uuid());
  }
  std::sort(old_uuids.begin(), old_uuids.end());
  replicas_.clear();
  bool has_new_replica = false;
  for (const TabletLocationsPB_ReplicaPB& r : replicas) {
    auto it = tservers.find(r.ts_info().permanent_uuid());
    CHECK(it != tservers.end());
    replicas_.emplace_back(it->second.get(), r.role());
    has_new_replica =
        has_new_replica ||
        !std::binary_search(old_uuids.begin(), old_uuids.end(), r.ts_info().permanent_uuid());
  }
  if (has_new_replica) {
    lookups_without_new_replicas_ = 0;
  } else {
    ++lookups_without_new_replicas_;
  }
  stale_ = false;
  refresh_time_.store(MonoTime::Now(), std::memory_order_release);
}

void RemoteTablet::MarkStale() {
  std::lock_guard<rw_spinlock> lock(mutex_);
  stale_ = true;
}

bool RemoteTablet::stale() const {
  SharedLock<rw_spinlock> lock(mutex_);
  return stale_;
}

void RemoteTablet::MarkAsSplit() {
  std::lock_guard<rw_spinlock> lock(mutex_);
  is_split_ = true;
}

bool RemoteTablet::is_split() const {
  SharedLock<rw_spinlock> lock(mutex_);
  return is_split_;
}

bool RemoteTablet::MarkReplicaFailed(RemoteTabletServer *ts, const Status& status) {
  std::lock_guard<rw_spinlock> lock(mutex_);
  VLOG_WITH_PREFIX(2) << "Current remote replicas in meta cache: "
                      << ReplicasAsStringUnlocked() << ". Replica " << ts->ToString()
                      << " has failed: " << status.ToString();
  for (RemoteReplica& rep : replicas_) {
    if (rep.ts == ts) {
      rep.MarkFailed();
      return true;
    }
  }
  return false;
}

int RemoteTablet::GetNumFailedReplicas() const {
  int failed = 0;
  SharedLock<rw_spinlock> lock(mutex_);
  for (const RemoteReplica& rep : replicas_) {
    if (rep.Failed()) {
      failed++;
    }
  }
  return failed;
}

RemoteTabletServer* RemoteTablet::LeaderTServer() const {
  SharedLock<rw_spinlock> lock(mutex_);
  for (const RemoteReplica& replica : replicas_) {
    if (!replica.Failed() && replica.role == RaftPeerPB::LEADER) {
      return replica.ts;
    }
  }
  return nullptr;
}

bool RemoteTablet::HasLeader() const {
  return LeaderTServer() != nullptr;
}

void RemoteTablet::GetRemoteTabletServers(
    std::vector<RemoteTabletServer*>* servers, IncludeFailedReplicas include_failed_replicas) {
  DCHECK(servers->empty());
  struct ReplicaUpdate {
    RemoteReplica* replica;
    tablet::RaftGroupStatePB new_state;
    bool clear_failed;
  };
  std::vector<ReplicaUpdate> replica_updates;
  {
    SharedLock<rw_spinlock> lock(mutex_);
    for (RemoteReplica& replica : replicas_) {
      if (replica.Failed()) {
        if (include_failed_replicas) {
          servers->push_back(replica.ts);
          continue;
        }
        ReplicaUpdate replica_update = {&replica, RaftGroupStatePB::UNKNOWN, false};
        VLOG_WITH_PREFIX(4)
            << "Replica " << replica.ts->ToString()
            << " failed, state: " << RaftGroupStatePB_Name(replica.state)
            << ", is local: " << replica.ts->IsLocal()
            << ", time since failure: " << (MonoTime::Now() - replica.last_failed_time);
        switch (replica.state) {
          case RaftGroupStatePB::UNKNOWN: FALLTHROUGH_INTENDED;
          case RaftGroupStatePB::NOT_STARTED: FALLTHROUGH_INTENDED;
          case RaftGroupStatePB::BOOTSTRAPPING: FALLTHROUGH_INTENDED;
          case RaftGroupStatePB::RUNNING:
            // These are non-terminal states that may retry. Check and update failed local replica's
            // current state. For remote replica, just wait for some time before retrying.
            if (replica.ts->IsLocal()) {
              tserver::GetTabletStatusRequestPB req;
              tserver::GetTabletStatusResponsePB resp;
              req.set_tablet_id(tablet_id_);
              const Status status =
                  CHECK_NOTNULL(replica.ts->local_tserver())->GetTabletStatus(&req, &resp);
              if (!status.ok() || resp.has_error()) {
                LOG_WITH_PREFIX(ERROR)
                    << "Received error from GetTabletStatus: "
                    << (!status.ok() ? status : StatusFromPB(resp.error().status()));
                continue;
              }

              DCHECK_EQ(resp.tablet_status().tablet_id(), tablet_id_);
              VLOG_WITH_PREFIX(3) << "GetTabletStatus returned status: "
                                  << tablet::RaftGroupStatePB_Name(resp.tablet_status().state())
                                  << " for replica " << replica.ts->ToString();
              replica_update.new_state = resp.tablet_status().state();
              if (replica_update.new_state != tablet::RaftGroupStatePB::RUNNING) {
                if (replica_update.new_state != replica.state) {
                  // Cannot update replica here directly because holding only shared lock on mutex.
                  replica_updates.push_back(replica_update); // Update only state
                }
                continue;
              }
              if (!replica.ts->local_tserver()->LeaderAndReady(
                      tablet_id_, /* allow_stale */ true)) {
                // Should continue here because otherwise failed state will be cleared.
                continue;
              }
            } else if ((MonoTime::Now() - replica.last_failed_time) <
                       FLAGS_retry_failed_replica_ms * 1ms) {
              continue;
            }
            break;
          case RaftGroupStatePB::FAILED: FALLTHROUGH_INTENDED;
          case RaftGroupStatePB::QUIESCING: FALLTHROUGH_INTENDED;
          case RaftGroupStatePB::SHUTDOWN:
            // These are terminal states, so we won't retry.
            continue;
        }

        VLOG_WITH_PREFIX(3) << "Changing state of replica " << replica.ts->ToString()
                            << " from failed to not failed";
        replica_update.clear_failed = true;
        // Cannot update replica here directly because holding only shared lock on mutex.
        replica_updates.push_back(replica_update);
      }
      servers->push_back(replica.ts);
    }
  }
  if (!replica_updates.empty()) {
    std::lock_guard<rw_spinlock> lock(mutex_);
    for (const auto& update : replica_updates) {
      if (update.new_state != RaftGroupStatePB::UNKNOWN) {
        update.replica->state = update.new_state;
      }
      if (update.clear_failed) {
        update.replica->ClearFailed();
      }
    }
  }
}

bool RemoteTablet::MarkTServerAsLeader(const RemoteTabletServer* server) {
  bool found = false;
  std::lock_guard<rw_spinlock> lock(mutex_);
  for (RemoteReplica& replica : replicas_) {
    if (replica.ts == server) {
      replica.role = RaftPeerPB::LEADER;
      found = true;
    } else if (replica.role == RaftPeerPB::LEADER) {
      replica.role = RaftPeerPB::FOLLOWER;
    }
  }
  VLOG_WITH_PREFIX(3) << "Latest replicas: " << ReplicasAsStringUnlocked();
  VLOG_IF_WITH_PREFIX(3, !found) << "Specified server not found: " << server->ToString()
                                 << ". Replicas: " << ReplicasAsStringUnlocked();
  return found;
}

void RemoteTablet::MarkTServerAsFollower(const RemoteTabletServer* server) {
  bool found = false;
  std::lock_guard<rw_spinlock> lock(mutex_);
  for (RemoteReplica& replica : replicas_) {
    if (replica.ts == server) {
      replica.role = RaftPeerPB::FOLLOWER;
      found = true;
    }
  }
  VLOG_WITH_PREFIX(3) << "Latest replicas: " << ReplicasAsStringUnlocked();
  DCHECK(found) << "Tablet " << tablet_id_ << ": Specified server not found: "
                << server->ToString() << ". Replicas: " << ReplicasAsStringUnlocked();
}

std::string RemoteTablet::ReplicasAsString() const {
  SharedLock<rw_spinlock> lock(mutex_);
  return ReplicasAsStringUnlocked();
}

std::string RemoteTablet::ReplicasAsStringUnlocked() const {
  DCHECK(mutex_.is_locked());
  string replicas_str;
  for (const RemoteReplica& rep : replicas_) {
    if (!replicas_str.empty()) replicas_str += ", ";
    replicas_str += rep.ToString();
  }
  return replicas_str;
}

std::string RemoteTablet::ToString() const {
  return YB_CLASS_TO_STRING(tablet_id, partition, split_depth);
}

////////////////////////////////////////////////////////////

MetaCache::MetaCache(YBClient* client)
  : client_(client),
    master_lookup_sem_(FLAGS_max_concurrent_master_lookups) {
}

MetaCache::~MetaCache() {
  Shutdown();
}

void MetaCache::Shutdown() {
  rpcs_.Shutdown();
}

void MetaCache::SetLocalTabletServer(const string& permanent_uuid,
                                     const shared_ptr<TabletServerServiceProxy>& proxy,
                                     const LocalTabletServer* local_tserver) {
  const auto entry = ts_cache_.emplace(permanent_uuid,
                                       std::make_unique<RemoteTabletServer>(permanent_uuid,
                                                                            proxy,
                                                                            local_tserver));
  CHECK(entry.second);
  local_tserver_ = entry.first->second.get();
}

void MetaCache::UpdateTabletServerUnlocked(const master::TSInfoPB& pb) {
  const std::string& permanent_uuid = pb.permanent_uuid();
  auto it = ts_cache_.find(permanent_uuid);
  if (it != ts_cache_.end()) {
    it->second->Update(pb);
    return;
  }

  VLOG(1) << "Client caching new TabletServer " << permanent_uuid;
  CHECK(ts_cache_.emplace(permanent_uuid, std::make_unique<RemoteTabletServer>(pb)).second);
}

// A (table, partition_key) --> tablet lookup. May be in-flight to a master, or
// may be handled locally.
//
// Keeps a reference on the owning metacache while alive.
class LookupRpc : public Rpc {
 public:
  LookupRpc(const scoped_refptr<MetaCache>& meta_cache,
            int64_t request_no,
            CoarseTimePoint deadline);

  virtual ~LookupRpc();

  void SendRpc() override;

  MetaCache* meta_cache() { return meta_cache_.get(); }
  YBClient* client() const { return meta_cache_->client_; }

  void ResetMasterLeaderAndRetry();

  virtual void NotifyFailure(const Status& status) = 0;

  std::shared_ptr<MasterServiceProxy> master_proxy() const {
    return client()->data_->master_proxy();
  }

  template <class Response>
  void DoFinished(const Status& status, const Response& resp,
                  const std::string* partition_group_start);

  std::string LogPrefix() const {
    return yb::ToString(this) + ": ";
  }

  int64_t request_no() const {
    return request_no_;
  }

  rpc::Rpcs::Handle* RpcHandle() {
    return &retained_self_;
  }

 private:
  virtual void DoSendRpc() = 0;

  void NewLeaderMasterDeterminedCb(const Status& status);

  const int64_t request_no_;

  // Pointer back to the tablet cache. Populated with location information
  // if the lookup finishes successfully.
  //
  // When the RPC is destroyed, a master lookup permit is returned to the
  // cache if one was acquired in the first place.
  scoped_refptr<MetaCache> meta_cache_;

  // Whether this lookup has acquired a master lookup permit.
  bool has_permit_ = false;

  rpc::Rpcs::Handle retained_self_;
};

LookupRpc::LookupRpc(const scoped_refptr<MetaCache>& meta_cache,
                     int64_t request_no,
                     CoarseTimePoint deadline)
    : Rpc(deadline, meta_cache->client_->messenger(), &meta_cache->client_->proxy_cache()),
      request_no_(request_no),
      meta_cache_(meta_cache),
      retained_self_(meta_cache_->rpcs_.InvalidHandle()) {
  DCHECK(deadline != CoarseTimePoint());
}

LookupRpc::~LookupRpc() {
  if (has_permit_) {
    meta_cache_->ReleaseMasterLookupPermit();
  }
}

void LookupRpc::SendRpc() {
  if (!has_permit_) {
    has_permit_ = meta_cache_->AcquireMasterLookupPermit();
  }
  if (!has_permit_) {
    // Couldn't get a permit, try again in a little while.
    ScheduleRetry(STATUS(TryAgain, "Client has too many outstanding requests to the master"));
    return;
  }

  // See YBClient::Data::SyncLeaderMasterRpc().
  auto now = CoarseMonoClock::Now();
  if (retrier().deadline() < now) {
    Finished(STATUS(TimedOut, "timed out after deadline expired"));
    return;
  }
  mutable_retrier()->PrepareController();

  DoSendRpc();
}

void LookupRpc::ResetMasterLeaderAndRetry() {
  client()->data_->SetMasterServerProxyAsync(
      retrier().deadline(),
      false /* skip_resolution */,
      true /* wait for leader election */,
      Bind(&LookupRpc::NewLeaderMasterDeterminedCb,
           Unretained(this)));
}

void LookupRpc::NewLeaderMasterDeterminedCb(const Status& status) {
  if (status.ok()) {
    mutable_retrier()->mutable_controller()->Reset();
    SendRpc();
  } else {
    LOG_WITH_PREFIX(WARNING) << "Failed to determine new Master: " << status;
    ScheduleRetry(status);
  }
}

template <class Response>
void LookupRpc::DoFinished(
    const Status& status, const Response& resp, const std::string* partition_group_start) {
  VLOG_WITH_FUNC(4) << "partition_group_start: "
                    << (partition_group_start ? Slice(*partition_group_start).ToDebugHexString()
                                              : "None");
  if (status.ok() && resp.has_error()) {
    LOG_WITH_PREFIX(INFO)
        << "Failed, got resp error " << master::MasterErrorPB::Code_Name(resp.error().code());
  } else if (!status.ok()) {
    LOG_WITH_PREFIX(INFO) << "Failed: " << status;
  }

  // Prefer early failures over controller failures.
  Status new_status = status;
  if (new_status.ok() &&
      mutable_retrier()->HandleResponse(this, &new_status, rpc::RetryWhenBusy::kFalse)) {
    return;
  }

  // Prefer controller failures over response failures.
  if (new_status.ok() && resp.has_error()) {
    new_status = StatusFromPB(resp.error().status());
    if (resp.error().code() == master::MasterErrorPB::NOT_THE_LEADER ||
        resp.error().code() == master::MasterErrorPB::CATALOG_MANAGER_NOT_INITIALIZED) {
      if (client()->IsMultiMaster()) {
        YB_LOG_EVERY_N_SECS(WARNING, 1) << "Leader Master has changed, re-trying...";
        ResetMasterLeaderAndRetry();
      } else {
        ScheduleRetry(new_status);
      }
      return;
    }
  }

  if (new_status.IsTimedOut()) {
    if (CoarseMonoClock::Now() < retrier().deadline()) {
      YB_LOG_EVERY_N_SECS(WARNING, 1) << "Leader Master timed out, re-trying...";
      ResetMasterLeaderAndRetry();
      return;
    } else {
      // Operation deadline expired during this latest RPC.
      new_status = new_status.CloneAndPrepend("timed out after deadline expired");
    }
  }

  if (new_status.IsNetworkError() || new_status.IsRemoteError()) {
    YB_LOG_EVERY_N_SECS(WARNING, 1) << "Encountered a error from the Master: "
         << new_status << ", retrying...";
    ResetMasterLeaderAndRetry();
    return;
  }

  // Prefer response failures over no tablets found.
  if (new_status.ok() && resp.tablet_locations_size() == 0) {
    new_status = STATUS(NotFound, "No such tablet found");
  }

  auto retained_self = meta_cache_->rpcs_.Unregister(&retained_self_);

  if (new_status.ok()) {
    new_status = meta_cache_->ProcessTabletLocations(
        resp.tablet_locations(), partition_group_start, request_no());
  }
  if (!new_status.ok()) {
    YB_LOG_WITH_PREFIX_EVERY_N_SECS(WARNING, 1) << new_status;
    new_status = new_status.CloneAndPrepend(Substitute("$0 failed", ToString()));
    NotifyFailure(new_status);
  }
}

namespace {

Status CheckTabletLocations(
    const google::protobuf::RepeatedPtrField<master::TabletLocationsPB>& locations) {
  const std::string* prev_partition_end = nullptr;
  for (const TabletLocationsPB& loc : locations) {
    if (prev_partition_end && *prev_partition_end > loc.partition().partition_key_start()) {
      LOG(DFATAL)
          << "There should be no overlaps in tablet partitions and they should be sorted "
          << "by partition_key_start. Prev partition end: "
          << Slice(*prev_partition_end).ToDebugHexString() << ", current partition start: "
          << Slice(loc.partition().partition_key_start()).ToDebugHexString()
          << ". Tablet locations: " << AsString(locations);
      return STATUS(IllegalState, "Wrong order or overlaps in partitions");
    }
    prev_partition_end = &loc.partition().partition_key_end();
  }
  return Status::OK();
}

class TabletIdLookup : public ToStringable {
 public:
  explicit TabletIdLookup(const TabletId& tablet_id) : tablet_id_(tablet_id) {}

  std::string ToString() const override {
    return Format("Tablet: $0", tablet_id_);
  }

 private:
  const TabletId& tablet_id_;
};

class TablePartitionLookup : public ToStringable {
 public:
  explicit TablePartitionLookup(const TableId& table_id, const std::string& partition_group_start)
      : table_id_(table_id), partition_group_start_(partition_group_start) {}

  std::string ToString() const override {
    return Format("Table: $0, partition: $1",
                  table_id_, Slice(partition_group_start_).ToDebugHexString());
  }

 private:
  const TableId& table_id_;
  const std::string& partition_group_start_;
};

} // namespace

Status MetaCache::ProcessTabletLocations(
    const google::protobuf::RepeatedPtrField<master::TabletLocationsPB>& locations,
    const std::string* partition_group_start,
    int64_t request_no) {
  if (VLOG_IS_ON(2)) {
    auto group_start =
        partition_group_start ? Slice(*partition_group_start).ToDebugHexString() : "<NULL>";
    for (const auto& loc : locations) {
      for (const auto& table_id : loc.table_ids()) {
        VLOG_WITH_FUNC(2) << loc.tablet_id() << ", " << table_id << "/" << group_start;
      }
    }
    VLOG_WITH_FUNC(4) << AsString(locations) << ", " << group_start;
  }

  RETURN_NOT_OK(CheckTabletLocations(locations));

  std::vector<std::pair<LookupTabletCallback, internal::RemoteTabletPtr>> to_notify;
  {
    std::lock_guard<decltype(mutex_)> lock(mutex_);

    std::unordered_map<TableId, std::unordered_map<std::string, RemoteTabletPtr>> processed_tables;

    for (const TabletLocationsPB& loc : locations) {
      const std::string& tablet_id = loc.tablet_id();
      // Next, update the tablet caches.
      RemoteTabletPtr remote = FindPtrOrNull(tablets_by_id_, tablet_id);

      // First, update the tserver cache, needed for the Refresh calls below.
      for (const TabletLocationsPB_ReplicaPB& r : loc.replicas()) {
        UpdateTabletServerUnlocked(r.ts_info());
      }

      for (const std::string& table_id : loc.table_ids()) {
        auto& processed_table = processed_tables[table_id];

        auto& table_data = tables_[table_id];
        auto& tablets_by_key = table_data.tablets_by_partition;

        if (remote) {
          // Partition should not have changed.
          DCHECK_EQ(loc.partition().partition_key_start(),
                    remote->partition().partition_key_start());
          DCHECK_EQ(loc.partition().partition_key_end(),
                    remote->partition().partition_key_end());

          // For colocated tables, RemoteTablet already exists because it was processed
          // in a previous iteration of the for loop (for loc.table_ids()).
          // We need to add this tablet to the current table's tablets_by_key map.
          tablets_by_key[remote->partition().partition_key_start()] = remote;

          VLOG(5) << "Refreshing tablet " << tablet_id << ": " << loc.ShortDebugString();
        } else {
          VLOG(5) << "Caching tablet " << tablet_id << ": " << loc.ShortDebugString();

          Partition partition;
          Partition::FromPB(loc.partition(), &partition);
          remote = new RemoteTablet(tablet_id, partition, loc.split_depth());

          CHECK(tablets_by_id_.emplace(tablet_id, remote).second);
          auto emplace_result = tablets_by_key.emplace(partition.partition_key_start(), remote);
          if (!emplace_result.second) {
            const auto& old_tablet = emplace_result.first->second;
            if (old_tablet->split_depth() < remote->split_depth()) {
              // Only replace with tablet of higher split_depth.
              // TODO(tsplit): add cleanup for table_data.split_tablets, we don't need to keep
              // pre-split tablet info after it has been fully covered by post-split tablets
              // in meta cache.
              table_data.split_tablets[partition.partition_key_start()].push_back(old_tablet);
              emplace_result.first->second = remote;
            } else {
              // If split_depth is the same - it should be the same tablet.
              if (old_tablet->split_depth() == loc.split_depth()
                  && old_tablet->tablet_id() != tablet_id) {
                const auto error_msg = Format(
                    "Can't replace tablet $0 with $1 at partition_key_start $2, split_depth $3",
                    old_tablet->tablet_id(), tablet_id, loc.partition().partition_key_start(),
                    old_tablet->split_depth());
                LOG(DFATAL) << error_msg;
                // Just skip updating this tablet for release build.
              }
            }
          }
          MaybeUpdateClientRequests(table_data, *remote);
        }
        remote->Refresh(ts_cache_, loc.replicas());

        if (partition_group_start) {
          processed_table.emplace(loc.partition().partition_key_start(), remote);
        }
      }

      auto it = tablet_lookups_by_id_.find(tablet_id);
      if (it != tablet_lookups_by_id_.end()) {
        while (auto* lookup = it->second.lookups.Pop()) {
          to_notify.emplace_back(std::move(lookup->callback), remote);
          delete lookup;
        }
        if (!partition_group_start) {
          it->second.Finished(request_no, TabletIdLookup(tablet_id));
        }
      }
    }

    if (partition_group_start) {
      for (const auto& processed_table : processed_tables) {
        auto& table_data = tables_[processed_table.first];
        const auto lookup_by_group_iter =
            table_data.tablet_lookups_by_group.find(*partition_group_start);
        if (lookup_by_group_iter != table_data.tablet_lookups_by_group.end()) {
          VLOG_WITH_FUNC(4) << "Checking tablet_lookups_by_group for partition_group_start: "
                            << Slice(*partition_group_start).ToDebugHexString();
          auto& lookups_group = lookup_by_group_iter->second;
          while (auto* lookup = lookups_group.lookups.Pop()) {
            auto remote_it = processed_table.second.find(*lookup->partition_start);
            to_notify.emplace_back(
                std::move(lookup->callback),
                remote_it != processed_table.second.end() ? remote_it->second : nullptr);
            delete lookup;
          }
          lookups_group.Finished(
              request_no, TablePartitionLookup(processed_table.first, *partition_group_start),
              processed_tables.size() != 1);
        }
      }
    }
  }

  for (const auto& callback_and_remote_tablet : to_notify) {
    if (callback_and_remote_tablet.second) {
      callback_and_remote_tablet.first(callback_and_remote_tablet.second);
    } else {
      static auto status = STATUS(TryAgain, "Tablet for requested partition is not yet running");
      callback_and_remote_tablet.first(status);
    }
  }

  return Status::OK();
}

RemoteTabletPtr MetaCache::GetNearestSplitAncestorUnlocked(
    const TableData& table_data, const RemoteTablet& tablet) {
  VLOG_WITH_FUNC(3) << Format("tablet: $0", tablet);
  if (tablet.split_depth() == 0) {
    // Tablet is not a result of another tablet split, it couldn't have split ancestor.
    return nullptr;
  }

  const auto& partition = tablet.partition();
  const auto& partition_key_start = partition.partition_key_start();

  VLOG_WITH_FUNC(4) << "table_data.tablets_by_partition: "
                    << AsString(table_data.tablets_by_partition);
  VLOG_WITH_FUNC(4) << "table_data.split_tablets: " << AsString(table_data.split_tablets);

  RemoteTabletPtr nearest_split_ancestor;

  {
    // Try to find the deepest tablet X in table_data.tablets_by_partition whose partition strictly
    // contains tablet's (T) partition.
    //
    // If such tablet X exists it will have the maximum possible X.partition_key_start <=
    // T.partition_key_start.
    //
    // Proof: Lets assume there is another tablet Y != X in the map that strictly contains
    // T.partition, but it has Y.partition_key_start < X.partition_key_start.
    // That means:
    // Y.partition_key_end >= T.partition_key_end > T.partition_key_start >= X.partition_key_start
    //  => Y.partition_key_end > X.partition_key_start
    // So, Y.partition overlaps with X.partition and Y.partition_key_start < X.partition_key_start.
    // Due to the nature of tablet splitting, there could be no partial overlap of tablet partition
    // key ranges. Then Y should strictly contains X and that means Y is not the
    // deepest tablet available strictly containing T.partition.
    const auto it = GetLastLessOrEqual(table_data.tablets_by_partition, partition_key_start);
    if (it != table_data.tablets_by_partition.end()) {
      VLOG_WITH_FUNC(3) << Format(
          "Nearest_split_ancestor candidate: $0",
          it != table_data.tablets_by_partition.end() ? AsString(it->second) : "None");
      if (it->second->partition().ContainsPartitionStrict(partition)) {
        nearest_split_ancestor = it->second;
        VLOG_WITH_FUNC(3) << Format(
            "Found nearest split ancestor tablet: $0", nearest_split_ancestor);
      }
    }
  }

  if (!nearest_split_ancestor) {
    // Nearest split ancestor could be already replaced and moved to table_data.split_tablets,
    // so try to find it there starting with largest depth.
    const auto it = GetLastLessOrEqual(table_data.split_tablets, partition_key_start);
    if (it != table_data.split_tablets.end()) {
      for (auto tablet_it = it->second.rbegin(); tablet_it != it->second.rend(); ++tablet_it) {
        VLOG_WITH_FUNC(3) << Format(
            "Nearest_split_ancestor candidate: $0", tablet_it->get());
        if (tablet_it->get()->partition().ContainsPartition(partition)) {
          nearest_split_ancestor = tablet_it->get();
          VLOG_WITH_FUNC(3) << Format(
              "Found nearest split ancestor tablet: $0", nearest_split_ancestor);
          break;
        }
      }
    }
  }

  if (!nearest_split_ancestor) {
    VLOG_WITH_FUNC(3) << Format("No nearest split ancestor tablet for: $0", tablet);
    return nullptr;
  }

  if (nearest_split_ancestor->split_depth() >= tablet.split_depth()) {
    LOG(DFATAL) << Format(
        "Nearest split ancestor $0 (split_depth: $1, partition: $2) should have smaller"
        " split depth than $3 (split_depth: $4, partition: $5)",
        nearest_split_ancestor->tablet_id(), nearest_split_ancestor->split_depth(),
        nearest_split_ancestor->partition(), tablet.tablet_id(), tablet.split_depth(),
        tablet.partition());
    return nullptr;
  }

  return nearest_split_ancestor;
}

void MetaCache::MaybeUpdateClientRequests(const TableData& table_data, const RemoteTablet& tablet) {
  const auto nearest_split_ancestor = GetNearestSplitAncestorUnlocked(table_data, tablet);

  if (!nearest_split_ancestor) {
    return;
  }

  // TODO: MetaCache is a friend of Client and tablet_requests_mutex_ with tablet_requests_ are
  // public members of YBClient::Data. Consider refactoring that.
  std::lock_guard<simple_spinlock> request_lock(client_->data_->tablet_requests_mutex_);
  auto& tablet_requests = client_->data_->tablet_requests_;
  const auto requests_it = tablet_requests.find(nearest_split_ancestor->tablet_id());
  if (requests_it == tablet_requests.end()) {
    LOG(DFATAL) << "Not found tablet requests structure for tablet "
                << nearest_split_ancestor->tablet_id();
    return;
  }
  tablet_requests[tablet.tablet_id()].request_id_seq = requests_it->second.request_id_seq;
}

void MetaCache::InvalidateTableCache(const TableId& table_id) {
  VLOG_WITH_FUNC(1) << "table: " << table_id;

  std::vector<LookupTabletCallback> to_notify;

  {
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    auto it = tables_.find(table_id);
    if (it != tables_.end()) {
      auto& table_data = it->second;
      // Some partitions could be mapped to tablets that have been split and we need to re-fetch
      // info about tablets serving partitions.
      for (auto& tablet : table_data.tablets_by_partition) {
        tablet.second->MarkStale();
      }
      // TODO(tsplit): Optimize to retry only necessary lookups inside ProcessTabletLocations,
      // detect which need to be retried by GetTableLocationsResponsePB.partitions_version.
      for (auto& group_lookups : table_data.tablet_lookups_by_group) {
        while (auto* lookup = group_lookups.second.lookups.Pop()) {
          to_notify.push_back(std::move(lookup->callback));
          delete lookup;
        }
      }
      table_data.tablet_lookups_by_group.clear();
    }
  }
  for (const auto& callback : to_notify) {
    callback(STATUS_FORMAT(TryAgain, "MetaCache for table $0 has been invalidated.", table_id));
  }
}

class MetaCache::CallbackNotifier {
 public:
  explicit CallbackNotifier(const Status& status) : status_(status) {}

  void Add(LookupTabletCallback&& callback) {
    callbacks_.push_back(std::move(callback));
  }

  ~CallbackNotifier() {
    for (const auto& callback : callbacks_) {
      callback(status_);
    }
  }
 private:
  std::vector<LookupTabletCallback> callbacks_;
  Status status_;
};

template <class Key>
CoarseTimePoint MetaCache::LookupFailed(
    const Key& key, const Status& status, int64_t request_no, const ToStringable& lookup_id,
    std::unordered_map<Key, LookupDataGroup>* key_to_group_lookup_data,
    CallbackNotifier* notifier) {
  auto key_to_group_lookup_data_iterator = key_to_group_lookup_data->find(key);
  if (key_to_group_lookup_data_iterator == key_to_group_lookup_data->end()) {
    return CoarseTimePoint();
  }
  auto& lookup_data_group = key_to_group_lookup_data_iterator->second;
  std::vector<LookupData*> retry;
  auto now = CoarseMonoClock::Now();
  CoarseTimePoint max_deadline;

  while (auto* lookup = lookup_data_group.lookups.Pop()) {
    if (!status.IsTimedOut() || lookup->deadline <= now) {
      notifier->Add(std::move(lookup->callback));
      delete lookup;
    } else {
      max_deadline = std::max(max_deadline, lookup->deadline);
      retry.push_back(lookup);
    }
  }

  if (retry.empty()) {
    lookup_data_group.Finished(request_no, lookup_id);
  } else {
    for (auto* lookup : retry) {
      lookup_data_group.lookups.Push(lookup);
    }
  }

  return max_deadline;
}

class LookupByIdRpc : public LookupRpc {
 public:
  LookupByIdRpc(const scoped_refptr<MetaCache>& meta_cache,
                const TabletId& tablet_id,
                int64_t request_no,
                CoarseTimePoint deadline,
                int64_t lookups_without_new_replicas)
      : LookupRpc(meta_cache, request_no, deadline),
        tablet_id_(tablet_id) {
    if (lookups_without_new_replicas != 0) {
      send_delay_ = std::min(
          lookups_without_new_replicas * FLAGS_meta_cache_lookup_throttling_step_ms,
          FLAGS_meta_cache_lookup_throttling_max_delay_ms) * 1ms;
    }
  }

  std::string ToString() const override {
    return Format("LookupByIdRpc(tablet: $0, num_attempts: $1)", tablet_id_, num_attempts());
  }

  void SendRpc() override {
    if (send_delay_) {
      auto delay = send_delay_;
      send_delay_ = MonoDelta();
      auto status = mutable_retrier()->DelayedRetry(this, Status::OK(), delay);
      if (!status.ok()) {
        Finished(status);
      }
      return;
    }

    LookupRpc::SendRpc();
  }

  void DoSendRpc() override {
    // Fill out the request.
    req_.clear_tablet_ids();
    req_.add_tablet_ids(tablet_id_);

    master_proxy()->GetTabletLocationsAsync(
        req_, &resp_, mutable_retrier()->mutable_controller(),
        std::bind(&LookupByIdRpc::Finished, this, Status::OK()));
  }

 private:
  void Finished(const Status& status) override {
    DoFinished(status, resp_, nullptr /* partition_group_start */);
  }

  void NotifyFailure(const Status& status) override {
    meta_cache()->LookupByIdFailed(tablet_id_, request_no(), status);
  }

  // Tablet to lookup.
  TabletId tablet_id_;

  // Request body.
  master::GetTabletLocationsRequestPB req_;

  // Response body.
  master::GetTabletLocationsResponsePB resp_;

  MonoDelta send_delay_;
};

class LookupByKeyRpc : public LookupRpc {
 public:
  LookupByKeyRpc(const scoped_refptr<MetaCache>& meta_cache,
                 const YBTable* table,
                 const MetaCache::PartitionGroupKey& partition_group_start,
                 int64_t request_no,
                 CoarseTimePoint deadline)
      : LookupRpc(meta_cache, request_no, deadline),
        table_(table->shared_from_this()),
        partition_group_start_(partition_group_start) {
  }

  std::string ToString() const override {
    return Format("GetTableLocations($0, $1, $2)",
                  table_->name(),
                  table_->partition_schema()
                      .PartitionKeyDebugString(partition_group_start_,
                                               internal::GetSchema(table_->schema())),
                  num_attempts());
  }

  const YBTableName& table_name() const { return table_->name(); }
  const string& table_id() const { return table_->id(); }

  void DoSendRpc() override {
    // Fill out the request.
    req_.mutable_table()->set_table_id(table_->id());
    req_.set_partition_key_start(partition_group_start_);
    req_.set_max_returned_locations(kPartitionGroupSize);

    // The end partition key is left unset intentionally so that we'll prefetch
    // some additional tablets.
    master_proxy()->GetTableLocationsAsync(
        req_, &resp_, mutable_retrier()->mutable_controller(),
        std::bind(&LookupByKeyRpc::Finished, this, Status::OK()));
  }

 private:
  void Finished(const Status& status) override {
    const auto table_partitions_version = table_->GetPartitionsVersion();
    VLOG_WITH_FUNC(4) << Format(
        "Received table $0 partitions version: $1, ours is: $2", table_->id(),
        resp_.partitions_version(), table_partitions_version);
    if (resp_.partitions_version() != table_partitions_version) {
      DoFinished(
          STATUS_EC_FORMAT(
              TryAgain,
              ClientError(
                  resp_.partitions_version() > table_partitions_version
                      ? ClientErrorCode::kTablePartitionsAreStale
                      : ClientErrorCode::kGotOldTablePartitions),
              "Received table $0 partitions version: $1, ours is: $2", table_->id(),
              resp_.partitions_version(), table_partitions_version),
          resp_, &partition_group_start_);
      return;
    }
    DoFinished(status, resp_, &partition_group_start_);
  }

  void NotifyFailure(const Status& status) override {
    meta_cache()->LookupByKeyFailed(table_.get(), partition_group_start_, request_no(), status);
  }

  // Table to lookup.
  std::shared_ptr<const YBTable> table_;

  // Encoded partition key to lookup.
  MetaCache::PartitionGroupKey partition_group_start_;

  // Request body.
  GetTableLocationsRequestPB req_;

  // Response body.
  GetTableLocationsResponsePB resp_;
};

void MetaCache::LookupByKeyFailed(
    const YBTable* table, const std::string& partition_group_start, int64_t request_no,
    const Status& status) {
  VLOG(1) << "Lookup for table " << table->id() << " and partition "
          << Slice(partition_group_start).ToDebugHexString() << ", failed with: " << status;

  CallbackNotifier notifier(status);
  CoarseTimePoint max_deadline;
  {
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    auto it = tables_.find(table->id());
    if (it == tables_.end()) {
      return;
    }

    max_deadline = LookupFailed(
        partition_group_start, status, request_no,
        TablePartitionLookup(table->id(), partition_group_start),
        &it->second.tablet_lookups_by_group, &notifier);
  }

  if (max_deadline != CoarseTimePoint()) {
    auto rpc = std::make_shared<LookupByKeyRpc>(
        this, table, partition_group_start, request_no, max_deadline);
    rpcs_.RegisterAndStart(rpc, rpc->RpcHandle());
  }
}

void MetaCache::LookupByIdFailed(
    const TabletId& tablet_id, int64_t request_no, const Status& status) {
  VLOG(1) << "Lookup for tablet " << tablet_id << ", failed with: " << status;

  CallbackNotifier notifier(status);
  CoarseTimePoint max_deadline;
  {
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    max_deadline = LookupFailed(tablet_id, status, request_no, TabletIdLookup(tablet_id),
                                &tablet_lookups_by_id_, &notifier);
  }

  if (max_deadline != CoarseTimePoint()) {
    auto rpc = std::make_shared<LookupByIdRpc>(this, tablet_id, request_no, max_deadline, 0);
    rpcs_.RegisterAndStart(rpc, rpc->RpcHandle());
  }
}

RemoteTabletPtr MetaCache::LookupTabletByKeyFastPathUnlocked(const YBTable* table,
                                                             const std::string& partition_key) {
  auto it = tables_.find(table->id());
  if (PREDICT_FALSE(it == tables_.end())) {
    // No cache available for this table.
    return nullptr;
  }

  DCHECK_EQ(partition_key, table->FindPartitionStart(partition_key));
  auto tablet_it = it->second.tablets_by_partition.find(partition_key);
  if (PREDICT_FALSE(tablet_it == it->second.tablets_by_partition.end())) {
    // No tablets with a start partition key lower than 'partition_key'.
    return nullptr;
  }

  const auto& result = tablet_it->second;

  // Stale entries must be re-fetched.
  if (result->stale()) {
    return nullptr;
  }

  if (result->partition().partition_key_end().compare(partition_key) > 0 ||
      result->partition().partition_key_end().empty()) {
    // partition_key < partition.end OR tablet doesn't end.
    return result;
  }

  return nullptr;
}

// We disable thread safety analysis in this function due to manual conditional locking.
RemoteTabletPtr MetaCache::FastLookupTabletByKeyUnlocked(
    const YBTable* table,
    const std::string& partition_start) {
  // Fast path: lookup in the cache.
  auto result = LookupTabletByKeyFastPathUnlocked(table, partition_start);
  if (result && result->HasLeader()) {
    VLOG(4) << "Fast lookup: found tablet " << result->tablet_id();
    return result;
  }

  return nullptr;
}

template <class Mutex>
bool IsUniqueLock(std::lock_guard<Mutex>*) {
  return true;
}

template <class Mutex>
bool IsUniqueLock(SharedLock<Mutex>*) {
  return false;
}

template <class Lock>
bool MetaCache::DoLookupTabletByKey(
    const YBTable* table, const std::string& partition_start, CoarseTimePoint deadline,
    LookupTabletCallback* callback, const std::string** partition_group_start) {
  RemoteTabletPtr tablet;
  auto scope_exit = ScopeExit([callback, &tablet] {
    if (tablet) {
      (*callback)(tablet);
    }
  });
  int64_t request_no;
  {
    Lock lock(mutex_);
    tablet = FastLookupTabletByKeyUnlocked(table, partition_start);
    if (tablet) {
      return true;
    }

    if (!*partition_group_start) {
      *partition_group_start = &table->FindPartitionStart(partition_start, kPartitionGroupSize);
    }

    auto table_it = tables_.find(table->id());
    if (table_it == tables_.end()) {
      if (!IsUniqueLock(&lock)) {
        return false;
      }
      table_it = tables_.emplace(table->id(), TableData()).first;
    }
    auto& tablet_lookups_by_group = table_it->second.tablet_lookups_by_group;
    LookupDataGroup* lookups_group;
    {
      auto lookups_group_it = tablet_lookups_by_group.find(**partition_group_start);
      if (lookups_group_it == tablet_lookups_by_group.end()) {
        if (!IsUniqueLock(&lock)) {
          return false;
        }
        lookups_group = &tablet_lookups_by_group[**partition_group_start];
      } else {
        lookups_group = &lookups_group_it->second;
      }
    }
    lookups_group->lookups.Push(new LookupData(callback, deadline, &partition_start));
    request_no = lookup_serial_.fetch_add(1, std::memory_order_acq_rel);
    int64_t expected = 0;
    if (!lookups_group->running_request_number.compare_exchange_strong(
            expected, request_no, std::memory_order_acq_rel)) {
      VLOG_WITH_FUNC(4)
          << "Lookup is already running for table: " << table->ToString()
          << ", partition_group_start: " << Slice(**partition_group_start).ToDebugHexString();
      return true;
    }
  }

  VLOG_WITH_FUNC(4)
      << "Start lookup for table: " << table->ToString()
      << ", partition_group_start: " << Slice(**partition_group_start).ToDebugHexString();

  auto rpc = std::make_shared<LookupByKeyRpc>(
      this, table, **partition_group_start, request_no, deadline);
  rpcs_.RegisterAndStart(rpc, rpc->RpcHandle());
  return true;
}

// We disable thread safety analysis in this function due to manual conditional locking.
void MetaCache::LookupTabletByKey(const YBTable* table,
                                  const string& partition_key,
                                  CoarseTimePoint deadline,
                                  LookupTabletCallback callback) {
  const auto& partition_start = table->FindPartitionStart(partition_key);
  VLOG_WITH_FUNC(4) << "Table: " << table->ToString()
                    << ", partition_key: " << Slice(partition_key).ToDebugHexString()
                    << ", partition_start: " << Slice(partition_start).ToDebugHexString();

  const std::string* partition_group_start = nullptr;
  if (DoLookupTabletByKey<SharedLock<boost::shared_mutex>>(
          table, partition_start, deadline, &callback, &partition_group_start)) {
    return;
  }

  bool result = DoLookupTabletByKey<std::lock_guard<boost::shared_mutex>>(
      table, partition_start, deadline, &callback, &partition_group_start);
  LOG_IF(DFATAL, !result)
      << "Lookup was not started for table " << table->ToString()
      << ", partition_key: " << Slice(partition_key).ToDebugHexString();
}

RemoteTabletPtr MetaCache::LookupTabletByIdFastPathUnlocked(const TabletId& tablet_id) {
  auto it = tablets_by_id_.find(tablet_id);
  if (it != tablets_by_id_.end()) {
    return it->second;
  }
  return nullptr;
}

template <class Lock>
bool MetaCache::DoLookupTabletById(
    const TabletId& tablet_id, CoarseTimePoint deadline, UseCache use_cache,
    LookupTabletCallback* callback) {
  RemoteTabletPtr tablet;
  auto scope_exit = ScopeExit([callback, &tablet] {
    if (tablet) {
      (*callback)(tablet);
    }
  });
  int64_t request_no;
  int64_t lookups_without_new_replicas = 0;
  {
    Lock lock(mutex_);

    // Fast path: lookup in the cache.
    tablet = LookupTabletByIdFastPathUnlocked(tablet_id);
    if (tablet) {
      if (use_cache && tablet->HasLeader()) {
        VLOG(4) << "Fast lookup: found tablet " << tablet->tablet_id();
        return true;
      }
      lookups_without_new_replicas = tablet->lookups_without_new_replicas();
      tablet = nullptr;
    }

    LookupDataGroup* lookup;
    {
      auto lookup_it = tablet_lookups_by_id_.find(tablet_id);
      if (lookup_it == tablet_lookups_by_id_.end()) {
        if (!IsUniqueLock(&lock)) {
          return false;
        }
        lookup = &tablet_lookups_by_id_[tablet_id];
      } else {
        lookup = &lookup_it->second;
      }
    }
    lookup->lookups.Push(new LookupData(callback, deadline, nullptr));
    request_no = lookup_serial_.fetch_add(1, std::memory_order_acq_rel);
    int64_t expected = 0;
    if (!lookup->running_request_number.compare_exchange_strong(
            expected, request_no, std::memory_order_acq_rel)) {
      VLOG_WITH_FUNC(4) << "Lookup already running for tablet: " << tablet_id;
      return true;
    }
  }

  VLOG_WITH_FUNC(4) << "Start lookup for tablet " << tablet_id << ": " << request_no;

  auto rpc = std::make_shared<LookupByIdRpc>(
      this, tablet_id, request_no, deadline, lookups_without_new_replicas);
  rpcs_.RegisterAndStart(rpc, rpc->RpcHandle());
  return true;
}

void MetaCache::LookupTabletById(const TabletId& tablet_id,
                                 CoarseTimePoint deadline,
                                 LookupTabletCallback callback,
                                 UseCache use_cache) {
  VLOG_WITH_FUNC(4) << "(" << tablet_id << ", " << use_cache << ")";

  if (DoLookupTabletById<SharedLock<decltype(mutex_)>>(
          tablet_id, deadline, use_cache, &callback)) {
    return;
  }

  auto result = DoLookupTabletById<std::lock_guard<decltype(mutex_)>>(
      tablet_id, deadline, use_cache, &callback);
  LOG_IF(DFATAL, !result) << "Lookup was not started for tablet " << tablet_id;
}

void MetaCache::MarkTSFailed(RemoteTabletServer* ts,
                             const Status& status) {
  LOG(INFO) << "Marking tablet server " << ts->ToString() << " as failed.";
  SharedLock<decltype(mutex_)> lock(mutex_);

  Status ts_status = status.CloneAndPrepend("TS failed");

  // TODO: replace with a ts->tablet multimap for faster lookup?
  for (const auto& tablet : tablets_by_id_) {
    // We just loop on all tablets; if a tablet does not have a replica on this
    // TS, MarkReplicaFailed() returns false and we ignore the return value.
    tablet.second->MarkReplicaFailed(ts, ts_status);
  }
}

bool MetaCache::AcquireMasterLookupPermit() {
  return master_lookup_sem_.TryAcquire();
}

void MetaCache::ReleaseMasterLookupPermit() {
  master_lookup_sem_.Release();
}

void MetaCache::LookupDataGroup::Finished(
    int64_t request_no, const ToStringable& id, bool allow_absence) {
  int64_t expected = request_no;
  if (!running_request_number.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
    if ((expected == 0 && max_completed_request_number <= request_no) && !allow_absence) {
      LOG(DFATAL) << "Lookup was not running for " << id.ToString() << ", expected: " << request_no;
    } else {
      LOG(INFO)
          << "Finished lookup for " << id.ToString() << ": " << request_no << ", while "
          << expected << " was running, could happen during tablet split";
    }
  } else {
    max_completed_request_number = std::max(max_completed_request_number, request_no);
    VLOG_WITH_FUNC(2) << "Finished lookup for " << id.ToString() << ", no: " << request_no;
  }
}

} // namespace internal
} // namespace client
} // namespace yb
