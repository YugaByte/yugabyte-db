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

#include "yb/consensus/peer_manager.h"

#include <mutex>

#include "yb/consensus/consensus_peers.h"
#include "yb/consensus/log.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/util/threadpool.h"
#include "yb/util/logging.h"

namespace yb {
namespace consensus {

using log::Log;
using strings::Substitute;

PeerManager::PeerManager(const std::string tablet_id,
                         const std::string local_uuid,
                         PeerProxyFactory* peer_proxy_factory,
                         PeerMessageQueue* queue,
                         ThreadPoolToken* raft_pool_token,
                         const scoped_refptr<log::Log>& log)
    : tablet_id_(tablet_id),
      local_uuid_(local_uuid),
      peer_proxy_factory_(peer_proxy_factory),
      queue_(queue),
      raft_pool_token_(raft_pool_token),
      log_(log) {
}

PeerManager::~PeerManager() {
  Close();
}

void PeerManager::UpdateRaftConfig(const RaftConfigPB& config) {
  VLOG_WITH_PREFIX(1) << "Updating peers from new config: " << config.ShortDebugString();

  std::lock_guard<simple_spinlock> lock(lock_);
  // Create new peers.
  for (const RaftPeerPB& peer_pb : config.peers()) {
    if (peers_.find(peer_pb.permanent_uuid()) != peers_.end()) {
      continue;
    }
    if (peer_pb.permanent_uuid() == local_uuid_) {
      continue;
    }

    VLOG_WITH_PREFIX(1) << "Adding remote peer. Peer: " << peer_pb.ShortDebugString();
    auto remote_peer = Peer::NewRemotePeer(
        peer_pb, tablet_id_, local_uuid_, peer_proxy_factory_->NewProxy(peer_pb), queue_,
        raft_pool_token_, consensus_, peer_proxy_factory_->messenger());
    if (!remote_peer.ok()) {
      LOG_WITH_PREFIX(WARNING)
          << "Failed to create remote peer for " << peer_pb.ShortDebugString() << ": "
          << remote_peer.status();
      return;
    }

    peers_[peer_pb.permanent_uuid()] = std::move(*remote_peer);
  }
}

void PeerManager::SignalRequest(RequestTriggerMode trigger_mode) {
  std::lock_guard<simple_spinlock> lock(lock_);
  for (auto iter = peers_.begin(); iter != peers_.end();) {
    Status s = iter->second->SignalRequest(trigger_mode);
    if (PREDICT_FALSE(s.IsIllegalState())) {
      LOG_WITH_PREFIX(WARNING)
          << "Peer was closed: " << s << ", removing from peers. Peer: "
          << (*iter).second->peer_pb().ShortDebugString();
      iter = peers_.erase(iter);
    } else {
      if (PREDICT_FALSE(!s.ok())) {
        LOG_WITH_PREFIX(WARNING)
            << "Peer " << (*iter).second->peer_pb().ShortDebugString()
            << " failed to send request: " << s;
      }
      iter++;
    }
  }
}

void PeerManager::Close() {
  std::lock_guard<simple_spinlock> lock(lock_);
  for (const auto& entry : peers_) {
    entry.second->Close();
  }
  peers_.clear();
}

void PeerManager::ClosePeersNotInConfig(const RaftConfigPB& config) {
  unordered_map<string, RaftPeerPB> peers_in_config;
  for (const RaftPeerPB &peer_pb : config.peers()) {
    InsertOrDie(&peers_in_config, peer_pb.permanent_uuid(), peer_pb);
  }

  std::lock_guard<simple_spinlock> lock(lock_);
  for (auto iter = peers_.begin(); iter != peers_.end();) {
    auto peer = iter->second.get();

    if (peer->peer_pb().permanent_uuid() == local_uuid_) {
      continue;
    }

    auto it = peers_in_config.find(peer->peer_pb().permanent_uuid());
    if (it == peers_in_config.end() ||
        it->second.member_type() != peer->peer_pb().member_type()) {
      peer->Close();
      iter = peers_.erase(iter);
    } else {
      iter++;
    }
  }
}

std::string PeerManager::LogPrefix() const {
  return MakeTabletLogPrefix(tablet_id_, local_uuid_);
}

} // namespace consensus
} // namespace yb
