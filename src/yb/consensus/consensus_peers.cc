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

#include "yb/consensus/consensus_peers.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <boost/optional.hpp>

#include "yb/common/wire_protocol.h"
#include "yb/consensus/consensus.h"
#include "yb/consensus/consensus.proxy.h"
#include "yb/consensus/consensus_error.h"
#include "yb/consensus/consensus_meta.h"
#include "yb/consensus/consensus_queue.h"
#include "yb/consensus/log.h"
#include "yb/consensus/replicate_msgs_holder.h"

#include "yb/gutil/strings/substitute.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/periodic.h"
#include "yb/tablet/tablet_error.h"
#include "yb/tserver/tserver.pb.h"
#include "yb/tserver/tserver_error.h"

#include "yb/util/backoff_waiter.h"
#include "yb/util/fault_injection.h"
#include "yb/util/flag_tags.h"
#include "yb/util/logging.h"
#include "yb/util/monotime.h"
#include "yb/util/net/net_util.h"
#include "yb/util/scope_exit.h"
#include "yb/util/status_callback.h"
#include "yb/util/threadpool.h"
#include "yb/util/tsan_util.h"

using namespace std::literals;
using namespace std::placeholders;

DEFINE_int32(consensus_rpc_timeout_ms, 3000,
             "Timeout used for all consensus internal RPC communications.");
TAG_FLAG(consensus_rpc_timeout_ms, advanced);

DEFINE_int32(max_wait_for_processresponse_before_closing_ms,
             yb::RegularBuildVsSanitizers(5000, 60000),
             "Maximum amount of time we will wait in Peer::Close() for Peer::ProcessResponse() to "
             "finish before returning proceding to close the Peer and return");
TAG_FLAG(max_wait_for_processresponse_before_closing_ms, advanced);

DECLARE_int32(raft_heartbeat_interval_ms);

DEFINE_test_flag(double, fault_crash_on_leader_request_fraction, 0.0,
                 "Fraction of the time when the leader will crash just before sending an "
                 "UpdateConsensus RPC.");

DEFINE_test_flag(int32, delay_removing_peer_with_failed_tablet_secs, 0,
                 "If greater than 0, Peer::ProcessResponse will sleep after receiving a response "
                 "indicating that a tablet is in the FAILED state, and before marking this peer "
                 "as failed.");

// Allow for disabling remote bootstrap in unit tests where we want to test
// certain scenarios without triggering bootstrap of a remote peer.
DEFINE_test_flag(bool, enable_remote_bootstrap, true,
                 "Whether remote bootstrap will be initiated by the leader when it "
                 "detects that a follower is out of date or does not have a tablet "
                 "replica.");

DECLARE_int32(TEST_log_change_config_every_n);

namespace yb {
namespace consensus {

using log::Log;
using log::LogEntryBatch;
using std::shared_ptr;
using rpc::Messenger;
using rpc::PeriodicTimer;
using rpc::RpcController;
using strings::Substitute;

Peer::Peer(
    const RaftPeerPB& peer_pb, string tablet_id, string leader_uuid, PeerProxyPtr proxy,
    PeerMessageQueue* queue, ThreadPoolToken* raft_pool_token, Consensus* consensus,
    rpc::Messenger* messenger)
    : tablet_id_(std::move(tablet_id)),
      leader_uuid_(std::move(leader_uuid)),
      peer_pb_(peer_pb),
      proxy_(std::move(proxy)),
      queue_(queue),
      raft_pool_token_(raft_pool_token),
      consensus_(consensus),
      messenger_(messenger) {}

void Peer::SetTermForTest(int term) {
  response_.set_responder_term(term);
}

Status Peer::Init() {
  std::lock_guard<simple_spinlock> lock(peer_lock_);
  queue_->TrackPeer(peer_pb_.permanent_uuid());
  // Capture a weak_ptr reference into the functor so it can safely handle
  // outliving the peer.
  std::weak_ptr<Peer> weak_peer = shared_from_this();
  heartbeater_ = PeriodicTimer::Create(
      messenger_,
      [weak_peer]() {
        if (auto p = weak_peer.lock()) {
          Status s = p->SignalRequest(RequestTriggerMode::kAlwaysSend);
        }
      },
      MonoDelta::FromMilliseconds(FLAGS_raft_heartbeat_interval_ms));
  heartbeater_->Start();
  state_ = kPeerStarted;
  return Status::OK();
}

Status Peer::SignalRequest(RequestTriggerMode trigger_mode) {
  // If the peer is currently sending, return Status::OK().
  // If there are new requests in the queue we'll get them on ProcessResponse().
  auto performing_lock = LockPerforming(std::try_to_lock);
  if (!performing_lock.owns_lock()) {
    return Status::OK();
  }

  {
    auto processing_lock = StartProcessingUnlocked();
    if (!processing_lock.owns_lock()) {
      return STATUS(IllegalState, "Peer was closed.");
    }

    // For the first request sent by the peer, we send it even if the queue is empty, which it will
    // always appear to be for the first request, since this is the negotiation round.
    if (PREDICT_FALSE(state_ == kPeerStarted)) {
      trigger_mode = RequestTriggerMode::kAlwaysSend;
      state_ = kPeerRunning;
    }
    DCHECK_EQ(state_, kPeerRunning);

    // If our last request generated an error, and this is not a normal heartbeat request (i.e.
    // we're not forcing a request even if the queue is empty, unlike we do during heartbeats),
    // then don't send the "per-RPC" request. Instead, we'll wait for the heartbeat.
    //
    // TODO: we could consider looking at the number of consecutive failed attempts, and instead of
    // ignoring the signal, ask the heartbeater to "expedite" the next heartbeat in order to achieve
    // something like exponential backoff after an error. As it is implemented today, any transient
    // error will result in a latency blip as long as the heartbeat period.
    if (failed_attempts_ > 0 && trigger_mode == RequestTriggerMode::kNonEmptyOnly) {
      return Status::OK();
    }

    using_thread_pool_.fetch_add(1, std::memory_order_acq_rel);
  }
  auto status = raft_pool_token_->SubmitFunc(
      std::bind(&Peer::SendNextRequest, shared_from_this(), trigger_mode));
  using_thread_pool_.fetch_sub(1, std::memory_order_acq_rel);
  if (status.ok()) {
    performing_lock.release();
  }
  return status;
}

void Peer::SendNextRequest(RequestTriggerMode trigger_mode) {
  auto retain_self = shared_from_this();
  DCHECK(performing_mutex_.is_locked()) << "Cannot send request";

  auto performing_lock = LockPerforming(std::adopt_lock);
  auto processing_lock = StartProcessingUnlocked();
  if (!processing_lock.owns_lock()) {
    return;
  }
  // Since there's a couple of return paths from this function, setup a cleanup, in case we fill in
  // ops inside request_, but do not get to use them.
  bool needs_cleanup = true;
  ScopeExit([&needs_cleanup, this](){
    if (needs_cleanup) {
      // Since we will not be using request_, we should cleanup the reserved ops.
      CleanRequestOps();
    }
  });

  // The peer has no pending request nor is sending: send the request.
  bool needs_remote_bootstrap = false;
  bool last_exchange_successful = false;
  RaftPeerPB::MemberType member_type = RaftPeerPB::UNKNOWN_MEMBER_TYPE;
  int64_t commit_index_before = request_.has_committed_op_id() ?
      request_.committed_op_id().index() : kMinimumOpIdIndex;
  ReplicateMsgsHolder msgs_holder;
  Status s = queue_->RequestForPeer(
      peer_pb_.permanent_uuid(), &request_, &msgs_holder, &needs_remote_bootstrap,
      &member_type, &last_exchange_successful);
  int64_t commit_index_after = request_.has_committed_op_id() ?
      request_.committed_op_id().index() : kMinimumOpIdIndex;

  if (PREDICT_FALSE(!s.ok())) {
    LOG_WITH_PREFIX(INFO) << "Could not obtain request from queue for peer: " << s;
    return;
  }

  if (PREDICT_FALSE(needs_remote_bootstrap)) {
    Status status;
    if (!FLAGS_TEST_enable_remote_bootstrap) {
      failed_attempts_++;
      status = STATUS(NotSupported, "remote bootstrap is disabled");
    } else {
      status = queue_->GetRemoteBootstrapRequestForPeer(peer_pb_.permanent_uuid(), &rb_request_);
    }
    if (!status.ok()) {
      LOG_WITH_PREFIX(WARNING) << "Unable to generate remote bootstrap request for peer: "
                               << status;
      return;
    }

    using_thread_pool_.fetch_add(1, std::memory_order_acq_rel);
    s = SendRemoteBootstrapRequest();
    using_thread_pool_.fetch_sub(1, std::memory_order_acq_rel);
    if (s.ok()) {
      performing_lock.release();
    }
    return;
  }

  // If the peer doesn't need remote bootstrap, but it is a PRE_VOTER or PRE_OBSERVER in the config,
  // we need to promote it.
  if (last_exchange_successful &&
      (member_type == RaftPeerPB::PRE_VOTER || member_type == RaftPeerPB::PRE_OBSERVER)) {
    if (PREDICT_TRUE(consensus_)) {
      auto uuid = peer_pb_.permanent_uuid();
      // Remove these here, before we drop the locks.
      needs_cleanup = false;
      CleanRequestOps();
      processing_lock.unlock();
      performing_lock.unlock();
      consensus::ChangeConfigRequestPB req;
      consensus::ChangeConfigResponsePB resp;

      req.set_tablet_id(tablet_id_);
      req.set_type(consensus::CHANGE_ROLE);
      RaftPeerPB *peer = req.mutable_server();
      peer->set_permanent_uuid(peer_pb_.permanent_uuid());

      boost::optional<tserver::TabletServerErrorPB::Code> error_code;

      // If another ChangeConfig is being processed, our request will be rejected.
      YB_LOG_EVERY_N(INFO, FLAGS_TEST_log_change_config_every_n)
          << "Sending ChangeConfig request to promote peer";
      auto status = consensus_->ChangeConfig(req, &DoNothingStatusCB, &error_code);
      if (PREDICT_FALSE(!status.ok())) {
        YB_LOG_EVERY_N(INFO, FLAGS_TEST_log_change_config_every_n)
            << "Unable to change role for peer " << uuid << ": " << status;
        // Since we released the semaphore, we need to call SignalRequest again to send a message
        status = SignalRequest(RequestTriggerMode::kAlwaysSend);
        if (PREDICT_FALSE(!status.ok())) {
          LOG(WARNING) << "Unexpected error when trying to send request: "
                       << status;
        }
      }
      return;
    }
  }

  if (request_.tablet_id().empty()) {
    request_.set_tablet_id(tablet_id_);
    request_.set_caller_uuid(leader_uuid_);
    request_.set_dest_uuid(peer_pb_.permanent_uuid());
  }

  const bool req_has_ops = (request_.ops_size() > 0) || (commit_index_after > commit_index_before);

  // If the queue is empty, check if we were told to send a status-only message (which is what
  // happens during heartbeats). If not, just return.
  if (PREDICT_FALSE(!req_has_ops && trigger_mode == RequestTriggerMode::kNonEmptyOnly)) {
    queue_->RequestWasNotSent(peer_pb_.permanent_uuid());
    return;
  }

  // If we're actually sending ops there's no need to heartbeat for a while, reset the heartbeater.
  if (req_has_ops) {
    heartbeater_->Snooze();
  }

  MAYBE_FAULT(FLAGS_TEST_fault_crash_on_leader_request_fraction);

  processing_lock.unlock();
  performing_lock.release();

  // We will cleanup ops from request in ProcessResponse, because otherwise there could be race
  // condition. When rest of this function is running in parallel to ProcessResponse.
  needs_cleanup = false;
  msgs_holder.ReleaseOps();

  controller_.set_invoke_callback_mode(rpc::InvokeCallbackMode::kThreadPoolHigh);
  proxy_->UpdateAsync(&request_, trigger_mode, &response_, &controller_,
                      std::bind(&Peer::ProcessResponse, retain_self));
}

std::unique_lock<simple_spinlock> Peer::StartProcessingUnlocked() {
  std::unique_lock<simple_spinlock> lock(peer_lock_);

  if (state_ == kPeerClosed) {
    lock.unlock();
  }

  return lock;
}

void Peer::ProcessResponse() {
  DCHECK(performing_mutex_.is_locked()) << "Got a response when nothing was pending";

  CleanRequestOps();

  Status status = controller_.status();
  if (status.ok()) {
    status = controller_.thread_pool_failure();
  }
  controller_.Reset();

  auto performing_lock = LockPerforming(std::adopt_lock);

  auto processing_lock = StartProcessingUnlocked();
  if (!processing_lock.owns_lock()) {
    return;
  }

  if (!status.ok()) {
    if (status.IsRemoteError()) {
      // Most controller errors are caused by network issues or corner cases like shutdown and
      // failure to serialize a protobuf. Therefore, we generally consider these errors to indicate
      // an unreachable peer.  However, a RemoteError wraps some other error propagated from the
      // remote peer, so we know the remote is alive. Therefore, we will let the queue know that the
      // remote is responsive.
      queue_->NotifyPeerIsResponsiveDespiteError(peer_pb_.permanent_uuid());
    }
    ProcessResponseError(status);
    return;
  }

  if (response_.has_propagated_hybrid_time()) {
    queue_->clock()->Update(HybridTime(response_.propagated_hybrid_time()));
  }

  // We should try to evict a follower which returns a WRONG UUID error.
  if (response_.has_error() &&
      response_.error().code() == tserver::TabletServerErrorPB::WRONG_SERVER_UUID) {
    queue_->NotifyObserversOfFailedFollower(
        peer_pb_.permanent_uuid(),
        Substitute("Leader communication with peer $0 received error $1, will try to "
                   "evict peer", peer_pb_.permanent_uuid(),
                   response_.error().ShortDebugString()));
    ProcessResponseError(StatusFromPB(response_.error().status()));
    return;
  }

  auto s = StatusFromResponse(response_);
  if (!s.ok() &&
      tserver::TabletServerError(s) == tserver::TabletServerErrorPB::TABLET_NOT_RUNNING &&
      tablet::RaftGroupStateError(s) == tablet::RaftGroupStatePB::FAILED) {
    if (PREDICT_FALSE(FLAGS_TEST_delay_removing_peer_with_failed_tablet_secs > 0)) {
      LOG(INFO) << "TEST: Sleeping for " << FLAGS_TEST_delay_removing_peer_with_failed_tablet_secs
                << " seconds";
      SleepFor(MonoDelta::FromSeconds(FLAGS_TEST_delay_removing_peer_with_failed_tablet_secs));
    }
    queue_->NotifyObserversOfFailedFollower(
        peer_pb_.permanent_uuid(),
        Format("Tablet in peer $0 is in FAILED state, will try to evict peer",
               peer_pb_.permanent_uuid()));
    ProcessResponseError(StatusFromPB(response_.error().status()));
  }

  // Response should be either error or status.
  LOG_IF(DFATAL, response_.has_error() == response_.has_status())
    << "Invalid response: " << response_.ShortDebugString();

  // Pass through errors we can respond to, like not found, since in that case
  // we will need to remotely bootstrap. TODO: Handle DELETED response once implemented.
  if ((response_.has_error() &&
      response_.error().code() != tserver::TabletServerErrorPB::TABLET_NOT_FOUND) ||
      (response_.status().has_error() &&
          response_.status().error().code() == consensus::ConsensusErrorPB::CANNOT_PREPARE)) {
    // Again, let the queue know that the remote is still responsive, since we will not be sending
    // this error response through to the queue.
    queue_->NotifyPeerIsResponsiveDespiteError(peer_pb_.permanent_uuid());
    ProcessResponseError(StatusFromPB(response_.error().status()));
    return;
  }

  failed_attempts_ = 0;
  bool more_pending = queue_->ResponseFromPeer(peer_pb_.permanent_uuid(), response_);

  if (more_pending) {
    processing_lock.unlock();
    performing_lock.release();
    SendNextRequest(RequestTriggerMode::kAlwaysSend);
  }
}

Status Peer::SendRemoteBootstrapRequest() {
  YB_LOG_WITH_PREFIX_EVERY_N_SECS(INFO, 30) << "Sending request to remotely bootstrap";
  controller_.set_invoke_callback_mode(rpc::InvokeCallbackMode::kThreadPoolNormal);
  return raft_pool_token_->SubmitFunc([retain_self = shared_from_this()]() {
    retain_self->proxy_->StartRemoteBootstrap(
      &retain_self->rb_request_, &retain_self->rb_response_, &retain_self->controller_,
      std::bind(&Peer::ProcessRemoteBootstrapResponse, retain_self));
  });
}

void Peer::ProcessRemoteBootstrapResponse() {
  Status status = controller_.status();
  controller_.Reset();

  auto performing_lock = LockPerforming(std::adopt_lock);
  auto processing_lock = StartProcessingUnlocked();
  if (!processing_lock.owns_lock()) {
    return;
  }

  if (!status.ok()) {
    LOG_WITH_PREFIX(WARNING) << "Unable to begin remote bootstrap on peer: " << status;
    return;
  }

  if (rb_response_.has_error()) {
    if (rb_response_.error().code() == tserver::TabletServerErrorPB::ALREADY_IN_PROGRESS) {
      queue_->NotifyPeerIsResponsiveDespiteError(peer_pb_.permanent_uuid());
      YB_LOG_WITH_PREFIX_EVERY_N_SECS(WARNING, 30)
        << ":::Unable to begin remote bootstrap on peer: " << rb_response_.ShortDebugString();
    } else {
      LOG_WITH_PREFIX(WARNING) << "Unable to begin remote bootstrap on peer: "
                               << rb_response_.ShortDebugString();
    }
  }
}

void Peer::ProcessResponseError(const Status& status) {
  DCHECK(performing_mutex_.is_locked());
  failed_attempts_++;
  YB_LOG_WITH_PREFIX_EVERY_N_SECS(WARNING, 5) << "Couldn't send request. "
      << " Status: " << status.ToString() << ". Retrying in the next heartbeat period."
      << " Already tried " << failed_attempts_ << " times. State: " << state_;
}

string Peer::LogPrefix() const {
  return Format("T $0 P $1 -> Peer $2 ($3, $4): ",
                tablet_id_, leader_uuid_, peer_pb_.permanent_uuid(),
                peer_pb_.last_known_private_addr(), peer_pb_.last_known_broadcast_addr());
}

void Peer::Close() {
  if (heartbeater_) {
    heartbeater_->Stop();
  }

  // If the peer is already closed return.
  {
    std::lock_guard<simple_spinlock> processing_lock(peer_lock_);
    if (using_thread_pool_.load(std::memory_order_acquire) > 0) {
      auto deadline = std::chrono::steady_clock::now() +
                      FLAGS_max_wait_for_processresponse_before_closing_ms * 1ms;
      BackoffWaiter waiter(deadline, 100ms);
      while (using_thread_pool_.load(std::memory_order_acquire) > 0) {
        if (!waiter.Wait()) {
          LOG_WITH_PREFIX(DFATAL)
              << "Timed out waiting for ThreadPoolToken::SubmitFunc() to finish. "
              << "Number of pending calls: " << using_thread_pool_.load(std::memory_order_acquire);
          break;
        }
      }
    }
    if (state_ == kPeerClosed) {
      return;
    }
    DCHECK(state_ == kPeerRunning || state_ == kPeerStarted) << "Unexpected state: " << state_;
    state_ = kPeerClosed;
    LOG_WITH_PREFIX(INFO) << "Closing peer";
  }

  auto retain_self = shared_from_this();

  queue_->UntrackPeer(peer_pb_.permanent_uuid());
}

Peer::~Peer() {
  std::lock_guard<simple_spinlock> processing_lock(peer_lock_);
  CHECK_EQ(state_, kPeerClosed) << "Peer cannot be implicitly closed";
}

void Peer::CleanRequestOps() {
  request_.mutable_ops()->ExtractSubrange(0, request_.ops().size(), nullptr /* elements */);
}

RpcPeerProxy::RpcPeerProxy(HostPort hostport, ConsensusServiceProxyPtr consensus_proxy)
    : hostport_(std::move(hostport)), consensus_proxy_(std::move(consensus_proxy)) {
}

void RpcPeerProxy::UpdateAsync(const ConsensusRequestPB* request,
                               RequestTriggerMode trigger_mode,
                               ConsensusResponsePB* response,
                               rpc::RpcController* controller,
                               const rpc::ResponseCallback& callback) {
  controller->set_timeout(MonoDelta::FromMilliseconds(FLAGS_consensus_rpc_timeout_ms));
  consensus_proxy_->UpdateConsensusAsync(*request, response, controller, callback);
}

void RpcPeerProxy::RequestConsensusVoteAsync(const VoteRequestPB* request,
                                             VoteResponsePB* response,
                                             rpc::RpcController* controller,
                                             const rpc::ResponseCallback& callback) {
  consensus_proxy_->RequestConsensusVoteAsync(*request, response, controller, callback);
}

void RpcPeerProxy::RunLeaderElectionAsync(const RunLeaderElectionRequestPB* request,
                                          RunLeaderElectionResponsePB* response,
                                          rpc::RpcController* controller,
                                          const rpc::ResponseCallback& callback) {
  controller->set_timeout(MonoDelta::FromMilliseconds(FLAGS_consensus_rpc_timeout_ms));
  consensus_proxy_->RunLeaderElectionAsync(*request, response, controller, callback);
}

void RpcPeerProxy::LeaderElectionLostAsync(const LeaderElectionLostRequestPB* request,
                                           LeaderElectionLostResponsePB* response,
                                           rpc::RpcController* controller,
                                           const rpc::ResponseCallback& callback) {
  consensus_proxy_->LeaderElectionLostAsync(*request, response, controller, callback);
}

void RpcPeerProxy::StartRemoteBootstrap(const StartRemoteBootstrapRequestPB* request,
                                        StartRemoteBootstrapResponsePB* response,
                                        rpc::RpcController* controller,
                                        const rpc::ResponseCallback& callback) {
  consensus_proxy_->StartRemoteBootstrapAsync(*request, response, controller, callback);
}

RpcPeerProxy::~RpcPeerProxy() {}

RpcPeerProxyFactory::RpcPeerProxyFactory(
    Messenger* messenger, rpc::ProxyCache* proxy_cache, CloudInfoPB from)
    : messenger_(messenger), proxy_cache_(proxy_cache), from_(std::move(from)) {}

PeerProxyPtr RpcPeerProxyFactory::NewProxy(const RaftPeerPB& peer_pb) {
  auto hostport = HostPortFromPB(DesiredHostPort(peer_pb, from_));
  auto proxy = std::make_unique<ConsensusServiceProxy>(proxy_cache_, hostport);
  return std::make_unique<RpcPeerProxy>(std::move(hostport), std::move(proxy));
}

RpcPeerProxyFactory::~RpcPeerProxyFactory() {}

rpc::Messenger* RpcPeerProxyFactory::messenger() const { return messenger_; }

struct GetNodeInstanceRequest {
  GetNodeInstanceRequestPB req;
  GetNodeInstanceResponsePB resp;
  rpc::RpcController controller;
  ConsensusServiceProxy proxy;

  GetNodeInstanceRequest(rpc::ProxyCache* proxy_cache, const HostPort& hostport)
      : proxy(proxy_cache, hostport) {}
};

Status SetPermanentUuidForRemotePeer(
    rpc::ProxyCache* proxy_cache,
    std::chrono::steady_clock::duration timeout,
    const std::vector<HostPort>& endpoints,
    RaftPeerPB* remote_peer) {

  DCHECK(!remote_peer->has_permanent_uuid());
  auto deadline = std::chrono::steady_clock::now() + timeout;

  std::vector<GetNodeInstanceRequest> requests;
  requests.reserve(endpoints.size());
  for (const auto& hp : endpoints) {
    requests.emplace_back(proxy_cache, hp);
  }

  CountDownLatch latch(requests.size());
  const auto kMaxWait = 10s;
  BackoffWaiter waiter(deadline, kMaxWait);
  for (;;) {
    latch.Reset(requests.size());
    std::atomic<GetNodeInstanceRequest*> last_reply{nullptr};
    for (auto& request : requests) {
      request.controller.Reset();
      request.controller.set_timeout(kMaxWait);
      VLOG(2) << "Getting uuid from remote peer. Request: " << request.req.ShortDebugString();

      request.proxy.GetNodeInstanceAsync(
          request.req, &request.resp, &request.controller,
          [&latch, &request, &last_reply] {
        if (!request.controller.status().IsTimedOut()) {
          last_reply.store(&request, std::memory_order_release);
        }
        latch.CountDown();
      });
    }

    latch.Wait();

    for (auto& request : requests) {
      auto status = request.controller.status();
      if (status.ok()) {
        remote_peer->set_permanent_uuid(request.resp.node_instance().permanent_uuid());
        remote_peer->set_member_type(RaftPeerPB::VOTER);
        if (request.resp.has_registration()) {
          CopyRegistration(request.resp.registration(), remote_peer);
        } else {
          // Required for backward compatibility.
          HostPortsToPBs(endpoints, remote_peer->mutable_last_known_private_addr());
        }
        return Status::OK();
      }
    }

    auto* last_reply_value = last_reply.load(std::memory_order_acquire);
    if (last_reply_value == nullptr) {
      last_reply_value = &requests.front();
    }

    LOG(WARNING) << "Error getting permanent uuid from config peer " << yb::ToString(endpoints)
                 << ": " << last_reply_value->controller.status();

    if (last_reply_value->controller.status().IsAborted()) {
      return last_reply_value->controller.status();
    }

    if (!waiter.Wait()) {
      return STATUS_FORMAT(
          TimedOut, "Getting permanent uuid from $0 timed out after $1: $2",
          endpoints, timeout, last_reply_value->controller.status());
    }

    LOG(INFO) << "Retrying to get permanent uuid for remote peer: "
              << yb::ToString(endpoints) << " attempt: " << waiter.attempt();
  }
}

}  // namespace consensus
}  // namespace yb
