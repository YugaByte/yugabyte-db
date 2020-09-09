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
// This module is internal to the client and not a public API.

#include "yb/master/master_rpc.h"

#include <mutex>

#include "yb/common/wire_protocol.h"
#include "yb/common/wire_protocol.pb.h"
#include "yb/gutil/bind.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/master/master.proxy.h"
#include "yb/util/net/net_util.h"
#include "yb/util/flag_tags.h"

using std::shared_ptr;
using std::string;
using std::vector;

using yb::consensus::RaftPeerPB;
using yb::rpc::Messenger;
using yb::rpc::Rpc;

using namespace std::placeholders;

DEFINE_int32(master_leader_rpc_timeout_ms, 500,
             "Number of milliseconds that the tserver will keep querying for master leader before"
             "selecting a follower.");
TAG_FLAG(master_leader_rpc_timeout_ms, advanced);
TAG_FLAG(master_leader_rpc_timeout_ms, hidden);

namespace yb {
namespace master {

namespace {

////////////////////////////////////////////////////////////
// GetMasterRegistrationRpc
////////////////////////////////////////////////////////////

// An RPC for getting a Master server's registration.
class GetMasterRegistrationRpc: public rpc::Rpc {
 public:

  // Create a wrapper object for a retriable GetMasterRegistration RPC
  // to 'addr'. The result is stored in 'out', which must be a valid
  // pointer for the lifetime of this object.
  //
  // Invokes 'user_cb' upon failure or success of the RPC call.
  GetMasterRegistrationRpc(StatusFunctor user_cb,
                           const HostPort& addr,
                           CoarseTimePoint deadline,
                           rpc::Messenger* messenger,
                           rpc::ProxyCache* proxy_cache,
                           ServerEntryPB* out)
      : Rpc(deadline, messenger, proxy_cache),
        user_cb_(std::move(user_cb)),
        addr_(addr),
        out_(DCHECK_NOTNULL(out)) {}

  void SendRpc() override;

  std::string ToString() const override;

 private:
  void Finished(const Status& status) override;

  StatusFunctor user_cb_;
  HostPort addr_;

  ServerEntryPB* out_;

  GetMasterRegistrationResponsePB resp_;
};

void GetMasterRegistrationRpc::SendRpc() {
  MasterServiceProxy proxy(&retrier().proxy_cache(), addr_);
  GetMasterRegistrationRequestPB req;
  proxy.GetMasterRegistrationAsync(
      req, &resp_, PrepareController(),
      std::bind(&GetMasterRegistrationRpc::Finished, this, Status::OK()));
}

string GetMasterRegistrationRpc::ToString() const {
  return Format("GetMasterRegistrationRpc(address: $0, num_attempts: $1, retries: $2)",
                addr_, num_attempts(), retrier());
}

void GetMasterRegistrationRpc::Finished(const Status& status) {
  Status new_status = status;
  if (new_status.ok() && mutable_retrier()->HandleResponse(this, &new_status)) {
    return;
  }
  if (new_status.ok() && resp_.has_error()) {
    if (resp_.error().code() == MasterErrorPB::CATALOG_MANAGER_NOT_INITIALIZED) {
      // If CatalogManager is not initialized, treat the node as a
      // FOLLOWER for the time being, as currently this RPC is only
      // used for the purposes of finding the leader master.
      resp_.set_role(RaftPeerPB::FOLLOWER);
      new_status = Status::OK();
    } else {
      out_->mutable_error()->CopyFrom(resp_.error().status());
      new_status = StatusFromPB(resp_.error().status());
    }
  }
  if (new_status.ok()) {
    out_->mutable_instance_id()->CopyFrom(resp_.instance_id());
    out_->mutable_registration()->CopyFrom(resp_.registration());
    out_->set_role(resp_.role());
  }
  auto callback = std::move(user_cb_);
  callback(new_status);
}

} // namespace

////////////////////////////////////////////////////////////
// GetLeaderMasterRpc
////////////////////////////////////////////////////////////

GetLeaderMasterRpc::GetLeaderMasterRpc(LeaderCallback user_cb,
                                       const server::MasterAddresses& addrs,
                                       CoarseTimePoint deadline,
                                       Messenger* messenger,
                                       rpc::ProxyCache* proxy_cache,
                                       rpc::Rpcs* rpcs,
                                       bool should_timeout_to_follower,
                                       bool wait_for_leader_election)
    : Rpc(deadline, messenger, proxy_cache),
      user_cb_(std::move(user_cb)),
      rpcs_(*rpcs),
      should_timeout_to_follower_(should_timeout_to_follower),
      wait_for_leader_election_(wait_for_leader_election) {
  DCHECK(deadline != CoarseTimePoint::max());

  for (const auto& list : addrs) {
    addrs_.insert(addrs_.end(), list.begin(), list.end());
  }
  // Using resize instead of reserve to explicitly initialized the values.
  responses_.resize(addrs_.size());
}

GetLeaderMasterRpc::~GetLeaderMasterRpc() {
}

string GetLeaderMasterRpc::ToString() const {
  return Format("GetLeaderMasterRpc(addrs: $0, num_attempts: $1)", addrs_, num_attempts());
}

void GetLeaderMasterRpc::SendRpc() {
  auto self = shared_from_this();

  size_t size = addrs_.size();
  std::vector<rpc::Rpcs::Handle> handles;
  handles.reserve(size);
  {
    std::lock_guard<simple_spinlock> l(lock_);
    pending_responses_ = size;
    for (int i = 0; i < size; i++) {
      auto handle = rpcs_.Prepare();
      if (handle == rpcs_.InvalidHandle()) {
        GetMasterRegistrationRpcCbForNode(i, STATUS(Aborted, "Stopping"), self, handle);
        continue;
      }
      *handle = std::make_shared<GetMasterRegistrationRpc>(
          std::bind(
              &GetLeaderMasterRpc::GetMasterRegistrationRpcCbForNode, this, i, _1, self, handle),
          addrs_[i],
          retrier().deadline(),
          retrier().messenger(),
          &retrier().proxy_cache(),
          &responses_[i]);
      handles.push_back(handle);
    }
  }

  for (const auto& handle : handles) {
    (**handle).SendRpc();
  }
}

void GetLeaderMasterRpc::Finished(const Status& status) {
  // If we've received replies from all of the nodes without finding
  // the leader, or if there were network errors talking to all of the
  // nodes the error is retriable and we can perform a delayed retry.
  num_iters_++;
  if (status.IsNetworkError() || (wait_for_leader_election_ && status.IsNotFound())) {
    VLOG(4) << "About to retry operation due to error: " << status.ToString();
    // TODO (KUDU-573): Allow cancelling delayed tasks on reactor so
    // that we can safely use DelayedRetry here.
    auto retry_status = mutable_retrier()->DelayedRetry(this, status);
    if (!retry_status.ok()) {
      LOG(WARNING) << "Failed to schedule retry: " << retry_status;
    } else {
      return;
    }
  }
  VLOG(4) << "Completed GetLeaderMasterRpc, calling callback with status "
          << status.ToString();
  {
    std::lock_guard<simple_spinlock> l(lock_);
    // 'completed_' prevents 'user_cb_' from being invoked twice.
    if (completed_) {
      return;
    }
    completed_ = true;
  }
  auto callback = std::move(user_cb_);
  user_cb_.Reset();
  callback.Run(status, leader_master_);
}

void GetLeaderMasterRpc::GetMasterRegistrationRpcCbForNode(
    int idx, const Status& status, const std::shared_ptr<rpc::RpcCommand>& self,
    rpc::Rpcs::Handle handle) {
  rpcs_.Unregister(handle);

  // TODO: handle the situation where one Master is partitioned from
  // the rest of the Master consensus configuration, all are reachable by the client,
  // and the partitioned node "thinks" it's the leader.
  //
  // The proper way to do so is to add term/index to the responses
  // from the Master, wait for majority of the Masters to respond, and
  // pick the one with the highest term/index as the leader.
  Status new_status = status;
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    --pending_responses_;
    if (completed_) {
      // If 'user_cb_' has been invoked (see Finished above), we can
      // stop.
      return;
    }
    auto& resp = responses_[idx];
    if (new_status.ok()) {
      if (resp.role() != RaftPeerPB::LEADER) {
        // Use a STATUS(NotFound, "") to indicate that the node is not
        // the leader: this way, we can handle the case where we've
        // received a reply from all of the nodes in the cluster (no
        // network or other errors encountered), but haven't found a
        // leader (which means that Finished() above can perform a
        // delayed retry).

        // If we have exceeded FLAGS_master_leader_rpc_timeout_ms, set this follower to be master
        // leader. This prevents an infinite retry loop when none of the master addresses passed in
        // are leaders.
        if (should_timeout_to_follower_ &&
            MonoTime::Now() - start_time_ >
            MonoDelta::FromMilliseconds(FLAGS_master_leader_rpc_timeout_ms)) {
          LOG(WARNING) << "More than " << FLAGS_master_leader_rpc_timeout_ms << " ms has passed, "
              "choosing to heartbeat to follower master " << resp.instance_id().permanent_uuid()
              << " after " << num_iters_ << " iterations of all masters.";
          leader_master_ = addrs_[idx];
        } else {
          new_status = STATUS(NotFound, "no leader found: " + ToString());
        }
      } else {
        // We've found a leader.
        leader_master_ = addrs_[idx];
      }
    }
    if (!new_status.ok()) {
      if (pending_responses_ > 0) {
        // Don't call Finished() on error unless we're the last
        // outstanding response: calling Finished() will trigger
        // a delayed re-try, which don't need to do unless we've
        // been unable to find a leader so far.
        return;
      }
    } else {
      completed_ = true;
    }
  }

  // Called if the leader has been determined, or if we've received
  // all of the responses.
  if (new_status.ok()) {
    auto callback = std::move(user_cb_);
    user_cb_.Reset();
    callback.Run(new_status, leader_master_);
  } else {
    Finished(new_status);
  }
}

} // namespace master
} // namespace yb
