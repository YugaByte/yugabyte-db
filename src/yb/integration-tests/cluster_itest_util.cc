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
#include "yb/integration-tests/cluster_itest_util.h"

#include <algorithm>
#include <limits>

#include <boost/optional.hpp>

#include <glog/stl_logging.h>

#include <gtest/gtest.h>

#include "yb/client/client.h"

#include "yb/common/wire_protocol.h"
#include "yb/common/wire_protocol.pb.h"
#include "yb/common/wire_protocol-test-util.h"

#include "yb/consensus/consensus_meta.h"
#include "yb/consensus/consensus.proxy.h"
#include "yb/consensus/opid_util.h"
#include "yb/consensus/quorum_util.h"

#include "yb/gutil/map-util.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/master/master.proxy.h"

#include "yb/rpc/rpc_controller.h"

#include "yb/server/server_base.proxy.h"
#include "yb/tserver/tablet_server_test_util.h"
#include "yb/tserver/tserver_admin.proxy.h"
#include "yb/tserver/tserver_service.pb.h"
#include "yb/tserver/tserver_service.proxy.h"

#include "yb/util/net/net_util.h"
#include "yb/util/test_util.h"

namespace yb {
namespace itest {

using client::YBClient;
using client::YBSchema;
using client::YBSchemaBuilder;
using client::YBTable;
using client::YBTableName;
using consensus::CONSENSUS_CONFIG_ACTIVE;
using consensus::CONSENSUS_CONFIG_COMMITTED;
using consensus::ChangeConfigRequestPB;
using consensus::ChangeConfigResponsePB;
using consensus::ConsensusConfigType;
using consensus::ConsensusStatePB;
using consensus::CountVoters;
using consensus::GetConsensusStateRequestPB;
using consensus::GetConsensusStateResponsePB;
using consensus::GetLastOpIdRequestPB;
using consensus::GetLastOpIdResponsePB;
using consensus::LeaderStepDownRequestPB;
using consensus::LeaderStepDownResponsePB;
using consensus::RaftPeerPB;
using consensus::RunLeaderElectionResponsePB;
using consensus::RunLeaderElectionRequestPB;
using consensus::kInvalidOpIdIndex;
using consensus::LeaderLeaseCheckMode;
using consensus::LeaderLeaseStatus;
using master::ListTabletServersResponsePB;
using master::MasterServiceProxy;
using master::TabletLocationsPB;
using rpc::Messenger;
using rpc::RpcController;
using std::min;
using std::shared_ptr;
using std::string;
using std::unordered_map;
using std::vector;
using strings::Substitute;
using tserver::CreateTsClientProxies;
using tserver::ListTabletsResponsePB;
using tserver::DeleteTabletRequestPB;
using tserver::DeleteTabletResponsePB;
using tserver::TabletServerAdminServiceProxy;
using tserver::TabletServerErrorPB;
using tserver::TabletServerServiceProxy;
using tserver::WriteRequestPB;
using tserver::WriteResponsePB;

const string& TServerDetails::uuid() const {
  return instance_id.permanent_uuid();
}

std::string TServerDetails::ToString() const {
  return Format("TabletServer: $0, Rpc address: $1", instance_id.permanent_uuid(),
                DesiredHostPort(registration.common(), CloudInfoPB()));
}

client::YBSchema SimpleIntKeyYBSchema() {
  YBSchema s;
  YBSchemaBuilder b;
  b.AddColumn("key")->Type(INT32)->NotNull()->PrimaryKey();
  CHECK_OK(b.Build(&s));
  return s;
}

Status GetLastOpIdForEachReplica(const string& tablet_id,
                                 const vector<TServerDetails*>& replicas,
                                 consensus::OpIdType opid_type,
                                 const MonoDelta& timeout,
                                 vector<OpIdPB>* op_ids) {
  GetLastOpIdRequestPB opid_req;
  GetLastOpIdResponsePB opid_resp;
  opid_req.set_tablet_id(tablet_id);
  RpcController controller;

  op_ids->clear();
  for (TServerDetails* ts : replicas) {
    controller.Reset();
    controller.set_timeout(timeout);
    opid_resp.Clear();
    opid_req.set_dest_uuid(ts->uuid());
    opid_req.set_tablet_id(tablet_id);
    opid_req.set_opid_type(opid_type);
    RETURN_NOT_OK_PREPEND(
      ts->consensus_proxy->GetLastOpId(opid_req, &opid_resp, &controller),
      Substitute("Failed to fetch last op id from $0",
                 ts->instance_id.ShortDebugString()));
    if (!opid_resp.has_opid()) {
      LOG(WARNING) << "Received uninitialized op id from " << ts->instance_id.ShortDebugString();
    }
    op_ids->push_back(opid_resp.opid());
  }

  return Status::OK();
}

Status GetLastOpIdForReplica(const std::string& tablet_id,
                             TServerDetails* replica,
                             consensus::OpIdType opid_type,
                             const MonoDelta& timeout,
                             OpIdPB* op_id) {
  vector<TServerDetails*> replicas;
  replicas.push_back(replica);
  vector<OpIdPB> op_ids;
  RETURN_NOT_OK(GetLastOpIdForEachReplica(tablet_id, replicas, opid_type, timeout, &op_ids));
  CHECK_EQ(1, op_ids.size());
  *op_id = op_ids[0];
  return Status::OK();
}

vector<TServerDetails*> TServerDetailsVector(const TabletServerMap& tablet_servers) {
  vector<TServerDetails*> result;
  result.reserve(tablet_servers.size());
  for (auto& pair : tablet_servers) {
    result.push_back(pair.second.get());
  }
  return result;
}

vector<TServerDetails*> TServerDetailsVector(const TabletServerMapUnowned& tablet_servers) {
  vector<TServerDetails*> result;
  result.reserve(tablet_servers.size());
  for (auto& pair : tablet_servers) {
    result.push_back(pair.second);
  }
  return result;
}

TabletServerMapUnowned CreateTabletServerMapUnowned(const TabletServerMap& tablet_servers,
                                                    const std::set<std::string>& exclude) {
  TabletServerMapUnowned result;
  for (auto& pair : tablet_servers) {
    if (exclude.find(pair.first) != exclude.end()) {
      continue;
    }
    result.emplace(pair.first, pair.second.get());
  }
  return result;
}

Status WaitForServersToAgree(const MonoDelta& timeout,
                             const TabletServerMap& tablet_servers,
                             const string& tablet_id,
                             int64_t minimum_index,
                             int64_t* actual_index,
                             MustBeCommitted must_be_committed) {
  return WaitForServersToAgree(timeout,
                               TServerDetailsVector(tablet_servers),
                               tablet_id,
                               minimum_index,
                               actual_index,
                               must_be_committed);
}

Status WaitForServersToAgree(const MonoDelta& timeout,
                             const TabletServerMapUnowned& tablet_servers,
                             const TabletId& tablet_id,
                             int64_t minimum_index,
                             int64_t* actual_index,
                             MustBeCommitted must_be_committed) {
  return WaitForServersToAgree(
      timeout, TServerDetailsVector(tablet_servers), tablet_id, minimum_index, actual_index,
      must_be_committed);
}

Status WaitForServersToAgree(const MonoDelta& timeout,
                             const vector<TServerDetails*>& servers,
                             const string& tablet_id,
                             int64_t minimum_index,
                             int64_t* actual_index,
                             MustBeCommitted must_be_committed) {
  auto deadline = CoarseMonoClock::Now() + timeout;
  if (actual_index != nullptr) {
    *actual_index = 0;
  }

  vector<OpIdType> opid_types{consensus::OpIdType::RECEIVED_OPID};
  if (must_be_committed) {
    // In this mode we require that last received and committed op ids from all servers converge
    // on the same value.
    opid_types.push_back(consensus::OpIdType::COMMITTED_OPID);
  }

  Status last_non_ok_status;
  vector<OpIdPB> received_ids;
  vector<OpIdPB> committed_ids;

  for (int attempt = 1; CoarseMonoClock::Now() < deadline; attempt++) {
    vector<OpIdPB> ids;

    Status s;
    for (auto opid_type : opid_types) {
      vector<OpIdPB> ids_of_this_type;
      s = GetLastOpIdForEachReplica(tablet_id, servers, opid_type, timeout, &ids_of_this_type);
      if (opid_type == consensus::OpIdType::RECEIVED_OPID) {
        received_ids = ids_of_this_type;
      } else {
        committed_ids = ids_of_this_type;
      }
      if (s.ok()) {
        std::copy(ids_of_this_type.begin(), ids_of_this_type.end(), std::back_inserter(ids));
      } else {
        break;
      }
    }

    if (s.ok()) {
      int64_t cur_index = kInvalidOpIdIndex;
      bool any_behind = false;
      bool any_disagree = false;
      for (const OpIdPB& id : ids) {
        if (cur_index == kInvalidOpIdIndex) {
          cur_index = id.index();
        }
        if (id.index() != cur_index) {
          any_disagree = true;
          break;
        }
        if (id.index() < minimum_index) {
          any_behind = true;
          break;
        }
      }
      if (!any_behind && !any_disagree) {
        LOG(INFO) << "All servers converged on OpIds: " << ids;
        if (actual_index != nullptr) {
          *actual_index = cur_index;
        }
        return Status::OK();
      }
    } else {
      LOG(WARNING) << "Got error getting last opid for each replica: " << s.ToString();
      last_non_ok_status = s;
    }

    LOG(INFO) << "Not converged past " << minimum_index << " yet: " << ids;
    SleepFor(MonoDelta::FromMilliseconds(min(attempt * 100, 1000)));
  }
  return STATUS_FORMAT(
      TimedOut,
      "All replicas of tablet $0 could not converge on an index of at least $1 after $2. "
      "must_be_committed=$3. Latest received ids: $3, committed ids: $4",
      tablet_id, minimum_index, timeout, must_be_committed, received_ids, committed_ids);
}

// Wait until all specified replicas have logged the given index.
Status WaitUntilAllReplicasHaveOp(const int64_t log_index,
                                  const string& tablet_id,
                                  const vector<TServerDetails*>& replicas,
                                  const MonoDelta& timeout,
                                  int64_t* actual_minimum_index) {
  MonoTime start = MonoTime::Now();
  MonoDelta passed = MonoDelta::FromMilliseconds(0);
  while (true) {
    vector<OpIdPB> op_ids;
    Status s = GetLastOpIdForEachReplica(tablet_id, replicas, consensus::RECEIVED_OPID, timeout,
                                         &op_ids);
    if (s.ok()) {
      if (actual_minimum_index != nullptr) {
        *actual_minimum_index = std::numeric_limits<int64_t>::max();
      }

      bool any_behind = false;
      for (const OpIdPB& op_id : op_ids) {
        if (actual_minimum_index != nullptr) {
          *actual_minimum_index = std::min(*actual_minimum_index, op_id.index());
        }

        if (op_id.index() < log_index) {
          any_behind = true;
          break;
        }
      }
      if (!any_behind) return Status::OK();
    } else {
      LOG(WARNING) << "Got error getting last opid for each replica: " << s.ToString();
    }
    passed = MonoTime::Now().GetDeltaSince(start);
    if (passed.MoreThan(timeout)) {
      break;
    }
    SleepFor(MonoDelta::FromMilliseconds(50));
  }
  string replicas_str;
  for (const TServerDetails* replica : replicas) {
    if (!replicas_str.empty()) replicas_str += ", ";
    replicas_str += "{ " + replica->ToString() + " }";
  }
  return STATUS(TimedOut, Substitute("Index $0 not available on all replicas after $1. "
                                              "Replicas: [ $2 ]",
                                              log_index, passed.ToString()));
}

Status WaitUntilNumberOfAliveTServersEqual(int n_tservers,
                                           MasterServiceProxy* master_proxy,
                                           const MonoDelta& timeout) {

  master::ListTabletServersRequestPB req;
  master::ListTabletServersResponsePB resp;
  rpc::RpcController controller;
  controller.set_timeout(timeout);

  // The field primary_only means only tservers that are alive (tservers that have sent at least on
  // heartbeat in the last FLAG_tserver_unresponsive_timeout_ms milliseconds.)
  req.set_primary_only(true);

  MonoTime start = MonoTime::Now();
  MonoDelta passed = MonoDelta::FromMilliseconds(0);
  while (true) {
    Status s = master_proxy->ListTabletServers(req, &resp, &controller);

    if (s.ok() &&
        controller.status().ok() &&
        !resp.has_error()) {
      if (resp.servers_size() == n_tservers) {
        passed = MonoTime::Now().GetDeltaSince(start);
        return Status::OK();
      }
    } else {
      string error;
      if (!s.ok()) {
        error = s.ToString();
      } else if (!controller.status().ok()) {
        error = controller.status().ToString();
      } else {
        error = resp.error().ShortDebugString();
      }
      LOG(WARNING) << "Got error getting list of tablet servers: " << error;
    }
    passed = MonoTime::Now().GetDeltaSince(start);
    if (passed.MoreThan(timeout)) {
      break;
    }
    SleepFor(MonoDelta::FromMilliseconds(50));
    controller.Reset();
  }
  return STATUS(TimedOut, Substitute("Number of alive tservers not equal to $0 after $1 ms. ",
                                     n_tservers, timeout.ToMilliseconds()));
}

Status CreateTabletServerMap(MasterServiceProxy* master_proxy,
                             rpc::ProxyCache* proxy_cache,
                             TabletServerMap* ts_map) {
  master::ListTabletServersRequestPB req;
  master::ListTabletServersResponsePB resp;
  rpc::RpcController controller;

  RETURN_NOT_OK(master_proxy->ListTabletServers(req, &resp, &controller));
  RETURN_NOT_OK(controller.status());
  if (resp.has_error()) {
    return STATUS(RemoteError, "Response had an error", resp.error().ShortDebugString());
  }

  ts_map->clear();
  for (const ListTabletServersResponsePB::Entry& entry : resp.servers()) {
    HostPort host_port = HostPortFromPB(DesiredHostPort(
        entry.registration().common(), CloudInfoPB()));

    std::unique_ptr<TServerDetails> peer(new TServerDetails());
    peer->instance_id.CopyFrom(entry.instance_id());
    peer->registration.CopyFrom(entry.registration());

    CreateTsClientProxies(host_port,
                          proxy_cache,
                          &peer->tserver_proxy,
                          &peer->tserver_admin_proxy,
                          &peer->consensus_proxy,
                          &peer->generic_proxy);

    const auto& key = peer->instance_id.permanent_uuid();
    CHECK(ts_map->emplace(key, std::move(peer)).second) << "duplicate key: " << key;
  }
  return Status::OK();
}

Status GetConsensusState(const TServerDetails* replica,
                         const string& tablet_id,
                         consensus::ConsensusConfigType type,
                         const MonoDelta& timeout,
                         ConsensusStatePB* consensus_state,
                         LeaderLeaseStatus* leader_lease_status) {
  DCHECK_ONLY_NOTNULL(replica);

  GetConsensusStateRequestPB req;
  GetConsensusStateResponsePB resp;
  RpcController controller;
  controller.set_timeout(timeout);
  req.set_dest_uuid(replica->uuid());
  req.set_tablet_id(tablet_id);
  req.set_type(type);

  RETURN_NOT_OK(replica->consensus_proxy->GetConsensusState(req, &resp, &controller));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }
  *consensus_state = resp.cstate();
  if (leader_lease_status) {
    *leader_lease_status = resp.has_leader_lease_status() ?
        resp.leader_lease_status() :
        LeaderLeaseStatus::NO_MAJORITY_REPLICATED_LEASE;  // Could be anything but HAS_LEASE.
  }
  return Status::OK();
}

Status WaitUntilCommittedConfigNumVotersIs(int config_size,
                                           const TServerDetails* replica,
                                           const std::string& tablet_id,
                                           const MonoDelta& timeout) {
  return WaitUntilCommittedConfigMemberTypeIs(config_size, replica, tablet_id, timeout,
                                              RaftPeerPB::VOTER);
}

Status WaitUntilCommittedConfigMemberTypeIs(int config_size,
                                           const TServerDetails* replica,
                                           const std::string& tablet_id,
                                           const MonoDelta& timeout,
                                           RaftPeerPB::MemberType member_type) {
  DCHECK_ONLY_NOTNULL(replica);

  MonoTime start = MonoTime::Now();
  MonoTime deadline = start + timeout;

  int backoff_exp = 0;
  const int kMaxBackoffExp = 7;
  Status s;
  ConsensusStatePB cstate;
  while (true) {
    MonoDelta remaining_timeout = deadline.GetDeltaSince(MonoTime::Now());
    s = GetConsensusState(replica, tablet_id, CONSENSUS_CONFIG_COMMITTED,
                          remaining_timeout, &cstate);
    if (s.ok()) {
      if (CountMemberType(cstate.config(), member_type) == config_size) {
        return Status::OK();
      }
    }

    if (MonoTime::Now().GetDeltaSince(start).MoreThan(timeout)) {
      break;
    }
    SleepFor(MonoDelta::FromMilliseconds(1 << backoff_exp));
    backoff_exp = min(backoff_exp + 1, kMaxBackoffExp);
  }
  return STATUS(TimedOut, Substitute("Number of replicas of type $0 does not equal $1 after "
                                     "waiting for $2. Last consensus state: $3. Last status: $4",
                                     RaftPeerPB::MemberType_Name(member_type), config_size,
                                     timeout.ToString(), cstate.ShortDebugString(), s.ToString()));
}

template<class Context>
Status WaitUntilCommittedOpIdIndex(TServerDetails* replica,
                                   const string& tablet_id,
                                   const MonoDelta& timeout,
                                   CommittedEntryType type,
                                   Context context) {
  MonoTime start = MonoTime::Now();
  MonoTime deadline = start;
  deadline.AddDelta(timeout);

  bool config = type == CommittedEntryType::CONFIG;
  Status s;
  OpIdPB op_id;
  ConsensusStatePB cstate;
  while (true) {
    MonoDelta remaining_timeout = deadline.GetDeltaSince(MonoTime::Now());

    int64_t op_index = -1;
    if (config) {
      s = GetConsensusState(replica, tablet_id, CONSENSUS_CONFIG_COMMITTED,
          remaining_timeout, &cstate);
      if (s.ok()) {
        op_index = cstate.config().opid_index();
      }
    } else {
      s = GetLastOpIdForReplica(tablet_id, replica, consensus::COMMITTED_OPID, remaining_timeout,
          &op_id);
      if (s.ok()) {
        op_index = op_id.index();
      }
    }

    if (s.ok() && context.Check(op_index)) {
      if (config) {
        LOG(INFO) << "Committed config state is: " << cstate.ShortDebugString() << " for replica: "
                  << replica->instance_id.permanent_uuid();
      } else {
        LOG(INFO) << "Committed op_id index is: " << op_id << " for replica: "
                  << replica->instance_id.permanent_uuid();
      }
      return Status::OK();
    }
    auto passed = MonoTime::Now().GetDeltaSince(start);
    if (passed.MoreThan(timeout)) {
      auto name = config ? "config" : "consensus";
      auto last_value = config ? cstate.ShortDebugString() : consensus::OpIdToString(op_id);
      return STATUS(TimedOut,
                    Substitute("Committed $0 opid_index does not equal $1 "
                               "after waiting for $2. Last value: $3, Last status: $4",
                               name,
                               context.Desired(),
                               passed.ToString(),
                               last_value,
                               s.ToString()));
    }
    if (!config) {
      LOG(INFO) << "Committed index is at: " << op_id.index() << " and not yet at "
                << context.Desired();
    }
    SleepFor(MonoDelta::FromMilliseconds(100));
  }
}

class WaitUntilCommittedOpIdIndexContext {
 public:
  explicit WaitUntilCommittedOpIdIndexContext(std::string desired)
      : desired_(std::move(desired)) {
  }

  const string& Desired() const {
    return desired_;
  }
 private:
  string desired_;
};

class WaitUntilCommittedOpIdIndexIsContext : public WaitUntilCommittedOpIdIndexContext {
 public:
  explicit WaitUntilCommittedOpIdIndexIsContext(int64_t value)
      : WaitUntilCommittedOpIdIndexContext(Substitute("equal $0", value)),
        value_(value) {
  }

  bool Check(int64_t current) {
    return value_ == current;
  }
 private:
  int64_t value_;
};

Status WaitUntilCommittedOpIdIndexIs(int64_t opid_index,
                                     TServerDetails* replica,
                                     const string& tablet_id,
                                     const MonoDelta& timeout,
                                     CommittedEntryType type) {
  return WaitUntilCommittedOpIdIndex(
      replica,
      tablet_id,
      timeout,
      type,
      WaitUntilCommittedOpIdIndexIsContext(opid_index));
}

class WaitUntilCommittedOpIdIndexIsGreaterThanContext : public WaitUntilCommittedOpIdIndexContext {
 public:
  explicit WaitUntilCommittedOpIdIndexIsGreaterThanContext(int64_t* value)
      : WaitUntilCommittedOpIdIndexContext(Substitute("greater than $0", *value)),
        original_value_(*value), value_(value) {

  }

  bool Check(int64_t current) {
    if (current > *value_) {
      CHECK_EQ(*value_, original_value_);
      *value_ = current;
      return true;
    }
    return false;
  }
 private:
  int64_t original_value_;
  int64_t* const value_;
};

Status WaitUntilCommittedOpIdIndexIsGreaterThan(int64_t* index,
                                                TServerDetails* replica,
                                                const TabletId& tablet_id,
                                                const MonoDelta& timeout,
                                                CommittedEntryType type) {
  return WaitUntilCommittedOpIdIndex(
      replica,
      tablet_id,
      timeout,
      type,
      WaitUntilCommittedOpIdIndexIsGreaterThanContext(index));
}

Status WaitUntilCommittedOpIdIndexIsAtLeast(int64_t* index,
                                            TServerDetails* replica,
                                            const TabletId& tablet_id,
                                            const MonoDelta& timeout,
                                            CommittedEntryType type) {
  int64_t tmp_index = *index - 1;
  Status s = WaitUntilCommittedOpIdIndexIsGreaterThan(
      &tmp_index,
      replica,
      tablet_id,
      timeout,
      type);
  *index = tmp_index;
  return s;
}

Status GetReplicaStatusAndCheckIfLeader(const TServerDetails* replica,
                                        const string& tablet_id,
                                        const MonoDelta& timeout,
                                        LeaderLeaseCheckMode lease_check_mode) {
  ConsensusStatePB cstate;
  LeaderLeaseStatus leader_lease_status;
  Status s = GetConsensusState(replica, tablet_id, CONSENSUS_CONFIG_ACTIVE,
                               timeout, &cstate, &leader_lease_status);
  if (PREDICT_FALSE(!s.ok())) {
    VLOG(1) << "Error getting consensus state from replica: "
            << replica->instance_id.permanent_uuid();
    return STATUS(NotFound, "Error connecting to replica", s.ToString());
  }
  const string& replica_uuid = replica->instance_id.permanent_uuid();
  if (cstate.has_leader_uuid() && cstate.leader_uuid() == replica_uuid &&
      (lease_check_mode == LeaderLeaseCheckMode::DONT_NEED_LEASE ||
       leader_lease_status == consensus::LeaderLeaseStatus::HAS_LEASE)) {
    return Status::OK();
  }
  VLOG(1) << "Replica not leader of config: " << replica->instance_id.permanent_uuid();
  return STATUS_FORMAT(IllegalState,
      "Replica found but not leader; lease check mode: $0", lease_check_mode);
}

Status WaitUntilLeader(const TServerDetails* replica,
                       const string& tablet_id,
                       const MonoDelta& timeout,
                       const LeaderLeaseCheckMode lease_check_mode) {
  MonoTime start = MonoTime::Now();
  MonoTime deadline = start;
  deadline.AddDelta(timeout);

  int backoff_exp = 0;
  const int kMaxBackoffExp = 7;
  Status s;
  while (true) {
    MonoDelta remaining_timeout = deadline.GetDeltaSince(MonoTime::Now());
    s = GetReplicaStatusAndCheckIfLeader(replica, tablet_id, remaining_timeout,
                                         lease_check_mode);
    if (s.ok()) {
      return Status::OK();
    }

    if (MonoTime::Now().GetDeltaSince(start).MoreThan(timeout)) {
      break;
    }
    SleepFor(MonoDelta::FromMilliseconds(1 << backoff_exp));
    backoff_exp = min(backoff_exp + 1, kMaxBackoffExp);
  }
  return STATUS(TimedOut, Substitute("Replica $0 is not leader after waiting for $1: $2",
                                     replica->ToString(), timeout.ToString(), s.ToString()));
}

Status FindTabletLeader(const TabletServerMap& tablet_servers,
                        const string& tablet_id,
                        const MonoDelta& timeout,
                        TServerDetails** leader) {
  return FindTabletLeader(TServerDetailsVector(tablet_servers), tablet_id, timeout, leader);
}

Status FindTabletLeader(const TabletServerMapUnowned& tablet_servers,
                        const string& tablet_id,
                        const MonoDelta& timeout,
                        TServerDetails** leader) {
  return FindTabletLeader(TServerDetailsVector(tablet_servers), tablet_id, timeout, leader);
}

Status FindTabletLeader(const vector<TServerDetails*>& tservers,
                        const string& tablet_id,
                        const MonoDelta& timeout,
                        TServerDetails** leader) {
  MonoTime start = MonoTime::Now();
  MonoTime deadline = start;
  deadline.AddDelta(timeout);
  Status s;
  int i = 0;
  while (true) {
    MonoDelta remaining_timeout = deadline.GetDeltaSince(MonoTime::Now());
    s = GetReplicaStatusAndCheckIfLeader(tservers[i], tablet_id, remaining_timeout);
    if (s.ok()) {
      *leader = tservers[i];
      return Status::OK();
    }

    if (deadline.ComesBefore(MonoTime::Now())) break;
    i = (i + 1) % tservers.size();
    if (i == 0) {
      SleepFor(MonoDelta::FromMilliseconds(10));
    }
  }
  return STATUS(TimedOut, Substitute("Unable to find leader of tablet $0 after $1. "
                                     "Status message: $2", tablet_id,
                                     MonoTime::Now().GetDeltaSince(start).ToString(),
                                     s.ToString()));
}

Status StartElection(const TServerDetails* replica,
                     const string& tablet_id,
                     const MonoDelta& timeout,
                     consensus::TEST_SuppressVoteRequest suppress_vote_request) {
  RunLeaderElectionRequestPB req;
  req.set_dest_uuid(replica->uuid());
  req.set_tablet_id(tablet_id);
  req.set_suppress_vote_request(suppress_vote_request);
  RunLeaderElectionResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(timeout);
  RETURN_NOT_OK(replica->consensus_proxy->RunLeaderElection(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status())
      .CloneAndPrepend(Substitute("Code $0", TabletServerErrorPB::Code_Name(resp.error().code())));
  }
  return Status::OK();
}

Status RequestVote(const TServerDetails* replica,
                   const std::string& tablet_id,
                   const std::string& candidate_uuid,
                   int64_t candidate_term,
                   const OpIdPB& last_logged_opid,
                   boost::optional<bool> ignore_live_leader,
                   boost::optional<bool> is_pre_election,
                   const MonoDelta& timeout) {
  RSTATUS_DCHECK(
      last_logged_opid.IsInitialized(), Uninitialized, "Last logged op id is uninitialized");
  consensus::VoteRequestPB req;
  req.set_dest_uuid(replica->uuid());
  req.set_tablet_id(tablet_id);
  req.set_candidate_uuid(candidate_uuid);
  req.set_candidate_term(candidate_term);
  *req.mutable_candidate_status()->mutable_last_received() = last_logged_opid;
  if (ignore_live_leader) req.set_ignore_live_leader(*ignore_live_leader);
  if (is_pre_election) req.set_preelection(*is_pre_election);
  consensus::VoteResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(timeout);
  RETURN_NOT_OK(replica->consensus_proxy->RequestConsensusVote(req, &resp, &rpc));
  if (resp.has_vote_granted() && resp.vote_granted())
    return Status::OK();
  if (resp.has_error())
    return StatusFromPB(resp.error().status());
  if (resp.has_consensus_error())
    return StatusFromPB(resp.consensus_error().status());
  return STATUS(IllegalState, "Unknown error (vote not granted)");
}

Status LeaderStepDown(
    const TServerDetails* replica,
    const string& tablet_id,
    const TServerDetails* new_leader,
    const MonoDelta& timeout,
    const bool disable_graceful_transition,
    TabletServerErrorPB* error) {
  LeaderStepDownRequestPB req;
  req.set_dest_uuid(replica->uuid());
  req.set_tablet_id(tablet_id);
  if (disable_graceful_transition) {
    req.set_disable_graceful_transition(disable_graceful_transition);
  }
  if (new_leader) {
    req.set_new_leader_uuid(new_leader->uuid());
  }
  LeaderStepDownResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(timeout);
  RETURN_NOT_OK(replica->consensus_proxy->LeaderStepDown(req, &resp, &rpc));
  if (resp.has_error()) {
    if (error != nullptr) {
      *error = resp.error();
    }
    return StatusFromPB(resp.error().status())
      .CloneAndPrepend(Substitute("Code $0", TabletServerErrorPB::Code_Name(resp.error().code())));
  }
  return WaitFor([&]() -> Result<bool> {
    rpc.Reset();
    GetConsensusStateRequestPB state_req;
    state_req.set_dest_uuid(replica->uuid());
    state_req.set_tablet_id(tablet_id);
    GetConsensusStateResponsePB state_resp;
    RETURN_NOT_OK(replica->consensus_proxy->GetConsensusState(state_req, &state_resp, &rpc));
    return state_resp.cstate().leader_uuid() != replica->uuid();
  }, timeout, "Leader change");
}

Status WriteSimpleTestRow(const TServerDetails* replica,
                          const std::string& tablet_id,
                          int32_t key,
                          int32_t int_val,
                          const string& string_val,
                          const MonoDelta& timeout) {
  WriteRequestPB req;
  WriteResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(timeout);

  req.set_tablet_id(tablet_id);

  AddTestRowInsert(key, int_val, string_val, &req);

  RETURN_NOT_OK(replica->tserver_proxy->Write(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }
  return Status::OK();
}

namespace {
  Status SendAddRemoveServerRequest(const TServerDetails* leader,
                                    const ChangeConfigRequestPB& req,
                                    ChangeConfigResponsePB* resp,
                                    RpcController* rpc,
                                    const MonoDelta& timeout,
                                    TabletServerErrorPB::Code* error_code,
                                    bool retry) {
    Status status = Status::OK();
    MonoTime start = MonoTime::Now();
    do {
      RETURN_NOT_OK(leader->consensus_proxy->ChangeConfig(req, resp, rpc));
      if (!resp->has_error()) {
        break;
      }
      if (error_code) *error_code = resp->error().code();
      status = StatusFromPB(resp->error().status());
      if (!retry) {
        break;
      }
      if (resp->error().code() != TabletServerErrorPB::LEADER_NOT_READY_CHANGE_CONFIG) {
        break;
      }
      rpc->Reset();
    } while (MonoTime::Now().GetDeltaSince(start).LessThan(timeout));
    return status;
  }
} // namespace

Status AddServer(const TServerDetails* leader,
                 const std::string& tablet_id,
                 const TServerDetails* replica_to_add,
                 consensus::RaftPeerPB::MemberType member_type,
                 const boost::optional<int64_t>& cas_config_opid_index,
                 const MonoDelta& timeout,
                 TabletServerErrorPB::Code* error_code,
                 bool retry) {
  ChangeConfigRequestPB req;
  ChangeConfigResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(timeout);

  req.set_dest_uuid(leader->uuid());
  req.set_tablet_id(tablet_id);
  req.set_type(consensus::ADD_SERVER);
  RaftPeerPB* peer = req.mutable_server();
  peer->set_permanent_uuid(replica_to_add->uuid());
  peer->set_member_type(member_type);
  CopyRegistration(replica_to_add->registration.common(), peer);
  if (cas_config_opid_index) {
    req.set_cas_config_opid_index(*cas_config_opid_index);
  }

  return SendAddRemoveServerRequest(leader, req, &resp, &rpc, timeout, error_code, retry);
}

Status RemoveServer(const TServerDetails* leader,
                    const std::string& tablet_id,
                    const TServerDetails* replica_to_remove,
                    const boost::optional<int64_t>& cas_config_opid_index,
                    const MonoDelta& timeout,
                    TabletServerErrorPB::Code* error_code,
                    bool retry) {
  ChangeConfigRequestPB req;
  ChangeConfigResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(timeout);

  req.set_dest_uuid(leader->uuid());
  req.set_tablet_id(tablet_id);
  req.set_type(consensus::REMOVE_SERVER);
  if (cas_config_opid_index) {
    req.set_cas_config_opid_index(*cas_config_opid_index);
  }
  RaftPeerPB* peer = req.mutable_server();
  peer->set_permanent_uuid(replica_to_remove->uuid());

  return SendAddRemoveServerRequest(leader, req, &resp, &rpc, timeout, error_code, retry);
}

Status ListTablets(const TServerDetails* ts,
                   const MonoDelta& timeout,
                   vector<ListTabletsResponsePB::StatusAndSchemaPB>* tablets) {
  tserver::ListTabletsRequestPB req;
  tserver::ListTabletsResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(timeout);

  RETURN_NOT_OK(ts->tserver_proxy->ListTablets(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  tablets->assign(resp.status_and_schema().begin(), resp.status_and_schema().end());
  return Status::OK();
}

Status ListRunningTabletIds(const TServerDetails* ts,
                            const MonoDelta& timeout,
                            vector<string>* tablet_ids) {
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  RETURN_NOT_OK(ListTablets(ts, timeout, &tablets));
  tablet_ids->clear();
  for (const ListTabletsResponsePB::StatusAndSchemaPB& t : tablets) {
    if (t.tablet_status().state() == tablet::RUNNING) {
      tablet_ids->push_back(t.tablet_status().tablet_id());
    }
  }
  return Status::OK();
}

Status GetTabletLocations(const shared_ptr<MasterServiceProxy>& master_proxy,
                          const string& tablet_id,
                          const MonoDelta& timeout,
                          master::TabletLocationsPB* tablet_locations) {
  master::GetTabletLocationsResponsePB resp;
  master::GetTabletLocationsRequestPB req;
  *req.add_tablet_ids() = tablet_id;
  rpc::RpcController rpc;
  rpc.set_timeout(timeout);
  RETURN_NOT_OK(master_proxy->GetTabletLocations(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }
  if (resp.errors_size() > 0) {
    CHECK_EQ(1, resp.errors_size()) << resp.ShortDebugString();
    return StatusFromPB(resp.errors(0).status());
  }
  CHECK_EQ(1, resp.tablet_locations_size()) << resp.ShortDebugString();
  *tablet_locations = resp.tablet_locations(0);
  return Status::OK();
}

Status GetTableLocations(const shared_ptr<MasterServiceProxy>& master_proxy,
                         const YBTableName& table_name,
                         const MonoDelta& timeout,
                         master::GetTableLocationsResponsePB* table_locations) {
  master::GetTableLocationsRequestPB req;
  table_name.SetIntoTableIdentifierPB(req.mutable_table());
  req.set_max_returned_locations(1000);
  rpc::RpcController rpc;
  rpc.set_timeout(timeout);
  RETURN_NOT_OK(master_proxy->GetTableLocations(req, table_locations, &rpc));
  if (table_locations->has_error()) {
    return StatusFromPB(table_locations->error().status());
  }
  return Status::OK();
}

Status WaitForNumVotersInConfigOnMaster(const shared_ptr<MasterServiceProxy>& master_proxy,
                                        const std::string& tablet_id,
                                        int num_voters,
                                        const MonoDelta& timeout) {
  Status s;
  MonoTime deadline = MonoTime::Now();
  deadline.AddDelta(timeout);
  int num_voters_found = 0;
  while (true) {
    TabletLocationsPB tablet_locations;
    MonoDelta time_remaining = deadline.GetDeltaSince(MonoTime::Now());
    s = GetTabletLocations(master_proxy, tablet_id, time_remaining, &tablet_locations);
    if (s.ok()) {
      num_voters_found = 0;
      for (const TabletLocationsPB::ReplicaPB& r : tablet_locations.replicas()) {
        if (r.role() == RaftPeerPB::LEADER || r.role() == RaftPeerPB::FOLLOWER) num_voters_found++;
      }
      if (num_voters_found == num_voters) break;
    }
    if (deadline.ComesBefore(MonoTime::Now())) break;
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  RETURN_NOT_OK(s);
  if (num_voters_found != num_voters) {
    return STATUS(IllegalState,
        Substitute("Did not find exactly $0 voters, found $1 voters",
                   num_voters, num_voters_found));
  }
  return Status::OK();
}

Status WaitForNumTabletsOnTS(TServerDetails* ts,
                             int count,
                             const MonoDelta& timeout,
                             vector<ListTabletsResponsePB::StatusAndSchemaPB>* tablets) {
  Status s;
  MonoTime deadline = MonoTime::Now();
  deadline.AddDelta(timeout);
  while (true) {
    s = ListTablets(ts, MonoDelta::FromSeconds(10), tablets);
    if (s.ok() && tablets->size() == count) break;
    if (deadline.ComesBefore(MonoTime::Now())) break;
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  RETURN_NOT_OK(s);
  if (tablets->size() != count) {
    return STATUS(IllegalState,
        Substitute("Did not find exactly $0 tablets, found $1 tablets",
                   count, tablets->size()));
  }
  return Status::OK();
}

Status WaitUntilTabletInState(TServerDetails* ts,
                              const std::string& tablet_id,
                              tablet::RaftGroupStatePB state,
                              const MonoDelta& timeout,
                              const MonoDelta& list_tablets_timeout) {
  MonoTime start = MonoTime::Now();
  MonoTime deadline = start;
  deadline.AddDelta(timeout);
  vector<ListTabletsResponsePB::StatusAndSchemaPB> tablets;
  Status s;
  tablet::RaftGroupStatePB last_state = tablet::UNKNOWN;
  while (true) {
    s = ListTablets(ts, list_tablets_timeout, &tablets);
    if (s.ok()) {
      bool seen = false;
      for (const ListTabletsResponsePB::StatusAndSchemaPB& t : tablets) {
        if (t.tablet_status().tablet_id() == tablet_id) {
          seen = true;
          last_state = t.tablet_status().state();
          if (last_state == state) {
            return Status::OK();
          }
        }
      }
      if (!seen) {
        s = STATUS(NotFound, "Tablet " + tablet_id + " not found");
      }
    }
    if (deadline.ComesBefore(MonoTime::Now())) {
      break;
    }
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  return STATUS(TimedOut, Substitute("T $0 P $1: Tablet not in $2 state after $3: "
                                     "Tablet state: $4, Status message: $5",
                                     tablet_id, ts->uuid(),
                                     tablet::RaftGroupStatePB_Name(state),
                                     MonoTime::Now().GetDeltaSince(start).ToString(),
                                     tablet::RaftGroupStatePB_Name(last_state), s.ToString()));
}

// Wait until the specified tablet is in RUNNING state.
Status WaitUntilTabletRunning(TServerDetails* ts,
                              const std::string& tablet_id,
                              const MonoDelta& timeout) {
  return WaitUntilTabletInState(ts, tablet_id, tablet::RUNNING, timeout);
}

Status DeleteTablet(const TServerDetails* ts,
                    const std::string& tablet_id,
                    const tablet::TabletDataState delete_type,
                    const boost::optional<int64_t>& cas_config_opid_index_less_or_equal,
                    const MonoDelta& timeout,
                    tserver::TabletServerErrorPB::Code* error_code) {
  DeleteTabletRequestPB req;
  DeleteTabletResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(timeout);

  req.set_dest_uuid(ts->uuid());
  req.set_tablet_id(tablet_id);
  req.set_delete_type(delete_type);
  if (cas_config_opid_index_less_or_equal) {
    req.set_cas_config_opid_index_less_or_equal(*cas_config_opid_index_less_or_equal);
  }

  RETURN_NOT_OK(ts->tserver_admin_proxy->DeleteTablet(req, &resp, &rpc));
  if (resp.has_error()) {
    if (error_code) {
      *error_code = resp.error().code();
    }
    return StatusFromPB(resp.error().status());
  }
  return Status::OK();
}

Status StartRemoteBootstrap(const TServerDetails* ts,
                            const string& tablet_id,
                            const string& bootstrap_source_uuid,
                            const HostPort& bootstrap_source_addr,
                            int64_t caller_term,
                            const MonoDelta& timeout) {
  consensus::StartRemoteBootstrapRequestPB req;
  consensus::StartRemoteBootstrapResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(timeout);

  req.set_dest_uuid(ts->uuid());
  req.set_tablet_id(tablet_id);
  req.set_bootstrap_peer_uuid(bootstrap_source_uuid);
  HostPortToPB(bootstrap_source_addr, req.mutable_source_private_addr()->Add());
  req.set_caller_term(caller_term);

  RETURN_NOT_OK(ts->consensus_proxy->StartRemoteBootstrap(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }
  return Status::OK();
}

Status GetLastOpIdForMasterReplica(const shared_ptr<ConsensusServiceProxy>& consensus_proxy,
                                   const string& tablet_id,
                                   const string& dest_uuid,
                                   const consensus::OpIdType opid_type,
                                   const MonoDelta& timeout,
                                   OpIdPB* opid) {
  GetLastOpIdRequestPB opid_req;
  GetLastOpIdResponsePB opid_resp;
  RpcController controller;
  controller.Reset();
  controller.set_timeout(timeout);

  opid_req.set_dest_uuid(dest_uuid);
  opid_req.set_tablet_id(tablet_id);
  opid_req.set_opid_type(opid_type);

  Status s = consensus_proxy->GetLastOpId(opid_req, &opid_resp, &controller);
  if (!s.ok()) {
    return STATUS(InvalidArgument, Substitute(
        "Failed to fetch opid type $0 from master uuid $1 with error : $2",
        opid_type, dest_uuid, s.ToString()));
  }
  if (opid_resp.has_error()) {
    return StatusFromPB(opid_resp.error().status());
  }

  *opid = opid_resp.opid();

  return Status::OK();
}

} // namespace itest
} // namespace yb
