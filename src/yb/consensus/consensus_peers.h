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

#ifndef YB_CONSENSUS_CONSENSUS_PEERS_H_
#define YB_CONSENSUS_CONSENSUS_PEERS_H_

#include <memory>
#include <string>
#include <vector>
#include <atomic>

#include "yb/consensus/consensus_fwd.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/metadata.pb.h"
#include "yb/consensus/consensus_util.h"

#include "yb/rpc/response_callback.h"
#include "yb/rpc/rpc_controller.h"

#include "yb/util/atomic.h"
#include "yb/util/countdown_latch.h"
#include "yb/util/locks.h"
#include "yb/util/net/net_util.h"
#include "yb/util/semaphore.h"
#include "yb/util/status.h"

namespace yb {
class HostPort;
class ThreadPoolToken;

namespace rpc {
class Messenger;
class PeriodicTimer;
}

namespace log {
class Log;
}

namespace consensus {

// A peer in consensus (local or remote).
//
// Leaders use peers to update the local Log and remote replicas.
//
// Peers are owned by the consensus implementation and do not keep state aside from whether there
// are requests pending or if requests are being processed.
//
// There are two external actions that trigger a state change:
//
// SignalRequest(): Called by the consensus implementation, notifies that the queue contains
// messages to be processed. This function takes a parameter allowing to send requests only if
// the queue is not empty, or to force-send a request even if it is empty.
//
// ProcessResponse() Called a response from a peer is received.
//
// The following state diagrams describe what happens when a state changing method is called.
//
//                        +
//                        |
//       SignalRequest()  |
//                        |
//                        |
//                        v
//              +------------------+
//       +------+    processing ?  +-----+
//       |      +------------------+     |
//       |                               |
//       | Yes                           | No
//       |                               |
//       v                               v
//     return                      ProcessNextRequest()
//                                 processing = true
//                                 - get reqs. from queue
//                                 - update peer async
//                                 return
//
//                         +
//                         |
//      ProcessResponse()  |
//      processing = false |
//                         v
//               +------------------+
//        +------+   more pending?  +-----+
//        |      +------------------+     |
//        |                               |
//        | Yes                           | No
//        |                               |
//        v                               v
//  SignalRequest()                    return
//
class Peer;
typedef std::shared_ptr<Peer> PeerPtr;

class Peer : public std::enable_shared_from_this<Peer> {
 public:
  Peer(const RaftPeerPB& peer, std::string tablet_id, std::string leader_uuid,
       PeerProxyPtr proxy, PeerMessageQueue* queue,
       ThreadPoolToken* raft_pool_token, Consensus* consensus, rpc::Messenger* messenger);

  // Initializes a peer and get its status.
  CHECKED_STATUS Init();

  // Signals that this peer has a new request to replicate/store.
  CHECKED_STATUS SignalRequest(RequestTriggerMode trigger_mode);

  const RaftPeerPB& peer_pb() const { return peer_pb_; }

  // Returns the PeerProxy if this is a remote peer or NULL if it
  // isn't. Used for tests to fiddle with the proxy and emulate remote
  // behavior.
  PeerProxy* GetPeerProxyForTests();

  // Stop sending requests and periodic heartbeats.
  //
  // This does not block waiting on any current outstanding requests to finish.
  // However, when they do finish, the results will be disregarded, so this
  // is safe to call at any point.
  //
  // This method must be called before the Peer's associated ThreadPoolToken
  // is destructed. Once this method returns, it is safe to destruct
  // the ThreadPoolToken.
  void Close();

  void SetTermForTest(int term);

  ~Peer();

  // Creates a new remote peer and makes the queue track it.'
  //
  // Requests to this peer (which may end up doing IO to read non-cached log entries) are assembled
  // on 'raft_pool_token'.  Response handling may also involve IO related to log-entry lookups
  // and is also done on 'thread_pool'.
  template <class... Args>
  static Result<PeerPtr> NewRemotePeer(Args&&... args) {
    auto new_peer = std::make_shared<Peer>(std::forward<Args>(args)...);
    RETURN_NOT_OK(new_peer->Init());
    return Result<PeerPtr>(std::move(new_peer));
  }

  uint64_t failed_attempts() {
    std::lock_guard<simple_spinlock> l(peer_lock_);
    return failed_attempts_;
  }

 private:
  void SendNextRequest(RequestTriggerMode trigger_mode);

  // Signals that a response was received from the peer. This method does response handling that
  // requires IO or may block.
  void ProcessResponse();

  // Fetch the desired remote bootstrap request from the queue and send it to the peer. The callback
  // goes to ProcessRemoteBootstrapResponse().
  //
  // Returns a bad Status if remote bootstrap is disabled, or if the request cannot be generated for
  // some reason.
  CHECKED_STATUS SendRemoteBootstrapRequest();

  // Handle RPC callback from initiating remote bootstrap.
  void ProcessRemoteBootstrapResponse();

  // Signals there was an error sending the request to the peer.
  void ProcessResponseError(const Status& status);

  // Returns true if the peer is closed and the calling function should return.
  std::unique_lock<simple_spinlock> StartProcessingUnlocked();

  template <class LockType>
  std::unique_lock<AtomicTryMutex> LockPerforming(LockType type) {
    return std::unique_lock<AtomicTryMutex>(performing_mutex_, type);
  }

  // Simple wrapper to cleanup ops from the request_.
  void CleanRequestOps();

  std::string LogPrefix() const;

  const std::string& tablet_id() const { return tablet_id_; }

  const std::string tablet_id_;
  const std::string leader_uuid_;

  const RaftPeerPB peer_pb_;

  PeerProxyPtr proxy_;

  PeerMessageQueue* queue_;
  uint64_t failed_attempts_ = 0;

  // The latest consensus update request and response.
  ConsensusRequestPB request_;
  ConsensusResponsePB response_;

  // The latest remote bootstrap request and response.
  StartRemoteBootstrapRequestPB rb_request_;
  StartRemoteBootstrapResponsePB rb_response_;

  rpc::RpcController controller_;

  // Held if there is an outstanding request.  This is used in order to ensure that we only have a
  // single request outstanding at a time, and to wait for the outstanding requests at Close().
  AtomicTryMutex performing_mutex_;

  // Heartbeater for remote peer implementations.  This will send status only requests to the remote
  // peers whenever we go more than 'FLAGS_raft_heartbeat_interval_ms' without sending actual data.
  std::shared_ptr<rpc::PeriodicTimer> heartbeater_;

  // Thread pool used to construct requests to this peer.
  ThreadPoolToken* raft_pool_token_;

  enum State {
    kPeerCreated,
    kPeerStarted,
    kPeerRunning,
    kPeerClosed
  };

  // Lock that protects Peer state changes, initialization, etc.  Must not try to acquire sem_ while
  // holding peer_lock_.
  mutable simple_spinlock peer_lock_;
  State state_ = kPeerCreated;
  Consensus* consensus_ = nullptr;
  rpc::Messenger* messenger_ = nullptr;
  std::atomic<int> using_thread_pool_{0};
};

// A proxy to another peer. Usually a thin wrapper around an rpc proxy but can be replaced for
// tests.
class PeerProxy {
 public:

  // Sends a request, asynchronously, to a remote peer.
  virtual void UpdateAsync(const ConsensusRequestPB* request,
                           RequestTriggerMode trigger_mode,
                           ConsensusResponsePB* response,
                           rpc::RpcController* controller,
                           const rpc::ResponseCallback& callback) = 0;

  // Sends a RequestConsensusVote to a remote peer.
  virtual void RequestConsensusVoteAsync(const VoteRequestPB* request,
                                         VoteResponsePB* response,
                                         rpc::RpcController* controller,
                                         const rpc::ResponseCallback& callback) = 0;

  // Instructs a peer to begin a remote bootstrap session.
  virtual void StartRemoteBootstrap(const StartRemoteBootstrapRequestPB* request,
                                    StartRemoteBootstrapResponsePB* response,
                                    rpc::RpcController* controller,
                                    const rpc::ResponseCallback& callback) {
    LOG(DFATAL) << "Not implemented";
  }

  // Sends a RunLeaderElection request to a peer.
  virtual void RunLeaderElectionAsync(const RunLeaderElectionRequestPB* request,
                                      RunLeaderElectionResponsePB* response,
                                      rpc::RpcController* controller,
                                      const rpc::ResponseCallback& callback) {
    LOG(DFATAL) << "Not implemented";
  }

  virtual void LeaderElectionLostAsync(const LeaderElectionLostRequestPB* request,
                                       LeaderElectionLostResponsePB* response,
                                       rpc::RpcController* controller,
                                       const rpc::ResponseCallback& callback) {
    LOG(DFATAL) << "Not implemented";
  }

  virtual ~PeerProxy() {}
};

typedef std::unique_ptr<PeerProxy> PeerProxyPtr;

// A peer proxy factory. Usually just obtains peers through the rpc implementation but can be
// replaced for tests.
class PeerProxyFactory {
 public:
  virtual PeerProxyPtr NewProxy(const RaftPeerPB& peer_pb) = 0;

  virtual ~PeerProxyFactory() {}

  virtual rpc::Messenger* messenger() const {
    return nullptr;
  }
};

// PeerProxy implementation that does RPC calls
class RpcPeerProxy : public PeerProxy {
 public:
  RpcPeerProxy(HostPort hostport, ConsensusServiceProxyPtr consensus_proxy);

  virtual void UpdateAsync(const ConsensusRequestPB* request,
                           RequestTriggerMode trigger_mode,
                           ConsensusResponsePB* response,
                           rpc::RpcController* controller,
                           const rpc::ResponseCallback& callback) override;

  virtual void RequestConsensusVoteAsync(const VoteRequestPB* request,
                                         VoteResponsePB* response,
                                         rpc::RpcController* controller,
                                         const rpc::ResponseCallback& callback) override;

  virtual void StartRemoteBootstrap(const StartRemoteBootstrapRequestPB* request,
                                    StartRemoteBootstrapResponsePB* response,
                                    rpc::RpcController* controller,
                                    const rpc::ResponseCallback& callback) override;

  virtual void RunLeaderElectionAsync(const RunLeaderElectionRequestPB* request,
                                      RunLeaderElectionResponsePB* response,
                                      rpc::RpcController* controller,
                                      const rpc::ResponseCallback& callback) override;

  virtual void LeaderElectionLostAsync(const LeaderElectionLostRequestPB* request,
                                       LeaderElectionLostResponsePB* response,
                                       rpc::RpcController* controller,
                                       const rpc::ResponseCallback& callback) override;

  virtual ~RpcPeerProxy();

 private:
  HostPort hostport_;
  ConsensusServiceProxyPtr consensus_proxy_;
};

// PeerProxyFactory implementation that generates RPCPeerProxies
class RpcPeerProxyFactory : public PeerProxyFactory {
 public:
  RpcPeerProxyFactory(rpc::Messenger* messenger, rpc::ProxyCache* proxy_cache, CloudInfoPB from);

  PeerProxyPtr NewProxy(const RaftPeerPB& peer_pb) override;

  virtual ~RpcPeerProxyFactory();

  rpc::Messenger* messenger() const override;

 private:
  rpc::Messenger* messenger_ = nullptr;
  rpc::ProxyCache* const proxy_cache_;
  const CloudInfoPB from_;
};

// Query the consensus service at last known host/port that is specified in 'remote_peer' and set
// the 'permanent_uuid' field based on the response.
Status SetPermanentUuidForRemotePeer(
    rpc::ProxyCache* proxy_cache,
    std::chrono::steady_clock::duration timeout,
    const std::vector<HostPort>& endpoints,
    RaftPeerPB* remote_peer);

}  // namespace consensus
}  // namespace yb

#endif /* YB_CONSENSUS_CONSENSUS_PEERS_H_ */
