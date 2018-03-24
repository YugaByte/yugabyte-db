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

#include "yb/integration-tests/external_mini_cluster.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include "yb/client/client.h"
#include "yb/client/table_handle.h"
#include "yb/common/wire_protocol.h"
#include "yb/fs/fs_manager.h"
#include "yb/gutil/mathlimits.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/strings/util.h"
#include "yb/gutil/singleton.h"
#include "yb/integration-tests/cluster_itest_util.h"
#include "yb/master/master.proxy.h"
#include "yb/master/master_rpc.h"
#include "yb/server/server_base.pb.h"
#include "yb/server/server_base.proxy.h"
#include "yb/tserver/tserver_service.proxy.h"
#include "yb/rpc/connection_context.h"
#include "yb/rpc/messenger.h"
#include "yb/master/sys_catalog.h"
#include "yb/util/async_util.h"
#include "yb/util/curl_util.h"
#include "yb/util/env.h"
#include "yb/util/jsonreader.h"
#include "yb/util/metrics.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/net/socket.h"
#include "yb/util/path_util.h"
#include "yb/util/pb_util.h"
#include "yb/util/stopwatch.h"
#include "yb/util/subprocess.h"
#include "yb/util/test_util.h"
#include "yb/util/size_literals.h"

using namespace std::literals;  // NOLINT

using std::atomic;
using std::lock_guard;
using std::mutex;
using std::shared_ptr;
using std::string;
using std::thread;
using std::unique_ptr;

using rapidjson::Value;
using strings::Substitute;

using yb::master::GetLeaderMasterRpc;
using yb::master::MasterServiceProxy;
using yb::server::ServerStatusPB;
using yb::tserver::ListTabletsRequestPB;
using yb::tserver::ListTabletsResponsePB;
using yb::tserver::TabletServerErrorPB;
using yb::tserver::TabletServerServiceProxy;
using yb::consensus::ConsensusServiceProxy;
using yb::consensus::RaftPeerPB;
using yb::consensus::ChangeConfigRequestPB;
using yb::consensus::ChangeConfigResponsePB;
using yb::consensus::ChangeConfigType;
using yb::consensus::GetLastOpIdRequestPB;
using yb::consensus::GetLastOpIdResponsePB;
using yb::consensus::LeaderStepDownRequestPB;
using yb::consensus::LeaderStepDownResponsePB;
using yb::consensus::RunLeaderElectionRequestPB;
using yb::consensus::RunLeaderElectionResponsePB;
using yb::master::IsMasterLeaderReadyRequestPB;
using yb::master::IsMasterLeaderReadyResponsePB;
using yb::master::ListMastersRequestPB;
using yb::master::ListMastersResponsePB;
using yb::master::ListMasterRaftPeersRequestPB;
using yb::master::ListMasterRaftPeersResponsePB;
using yb::tserver::TabletServerErrorPB;
using yb::rpc::RpcController;

typedef ListTabletsResponsePB::StatusAndSchemaPB StatusAndSchemaPB;

DECLARE_string(vmodule);
DECLARE_int32(replication_factor);
DECLARE_bool(mem_tracker_logging);
DECLARE_bool(mem_tracker_log_stack_trace);
DECLARE_string(minicluster_daemon_id);

DEFINE_string(external_daemon_heap_profile_prefix, "",
              "If this is not empty, tcmalloc's HEAPPROFILE is set this, followed by a unique "
              "suffix for external mini-cluster daemons.");

DEFINE_bool(external_daemon_safe_shutdown, false,
            "Shutdown external daemons using SIGTERM first. Disabled by default to avoid "
            "interfering with kill-testing.");

namespace yb {

static const char* const kMasterBinaryName = "yb-master";
static const char* const kTabletServerBinaryName = "yb-tserver";
static double kProcessStartTimeoutSeconds = 60.0;
static double kTabletServerRegistrationTimeoutSeconds = 10.0;

static const int kHeapProfileSignal = SIGUSR1;

#if defined(__APPLE__)
static bool kBindToUniqueLoopbackAddress = false;
#else
static bool kBindToUniqueLoopbackAddress = true;
#endif

constexpr size_t kDefaultMemoryLimitHardBytes = NonTsanVsTsan(1_GB, 512_MB);

ExternalMiniClusterOptions::ExternalMiniClusterOptions()
    : bind_to_unique_loopback_addresses(kBindToUniqueLoopbackAddress),
      timeout_(MonoDelta::FromMilliseconds(1000 * 10)) {
  if (bind_to_unique_loopback_addresses && sizeof(pid_t) > 2) {
    LOG(WARNING) << "pid size is " << sizeof(pid_t)
                 << ", setting bind_to_unique_loopback_addresses=false";
    bind_to_unique_loopback_addresses = false;
  }
}

ExternalMiniClusterOptions::~ExternalMiniClusterOptions() {
}

ExternalMiniCluster::ExternalMiniCluster(const ExternalMiniClusterOptions& opts)
    : opts_(opts), add_new_master_at_(-1) {
  // These "extra mini cluster options" are added in the end of the command line.
  const auto common_extra_flags = {
      "--enable_tracing"s,
      Substitute("--memory_limit_hard_bytes=$0", kDefaultMemoryLimitHardBytes)
  };
  for (auto* extra_flags : {&opts_.extra_master_flags, &opts_.extra_tserver_flags}) {
    // Common default extra flags are inserted in the beginning so that they can be overridden by
    // caller-specified flags.
    extra_flags->insert(extra_flags->begin(),
                        common_extra_flags.begin(),
                        common_extra_flags.end());
  }
}

ExternalMiniCluster::~ExternalMiniCluster() {
  Shutdown();
}

Status ExternalMiniCluster::DeduceBinRoot(std::string* ret) {
  string exe;
  RETURN_NOT_OK(Env::Default()->GetExecutablePath(&exe));
  *ret = DirName(exe) + "/../bin";
  return Status::OK();
}

Status ExternalMiniCluster::HandleOptions() {
  daemon_bin_path_ = opts_.daemon_bin_path;
  if (daemon_bin_path_.empty()) {
    RETURN_NOT_OK(DeduceBinRoot(&daemon_bin_path_));
  }

  data_root_ = opts_.data_root;
  if (data_root_.empty()) {
    // If they don't specify a data root, use the current gtest directory.
    data_root_ = JoinPathSegments(GetTestDataDirectory(), "minicluster-data");
  }

  return Status::OK();
}

Status ExternalMiniCluster::Start() {
  CHECK(masters_.empty()) << "Masters are not empty (size: " << masters_.size()
      << "). Maybe you meant Restart()?";
  CHECK(tablet_servers_.empty()) << "Tablet servers are not empty (size: "
      << tablet_servers_.size() << "). Maybe you meant Restart()?";
  RETURN_NOT_OK(HandleOptions());
  FLAGS_replication_factor = opts_.num_masters;

  rpc::MessengerBuilder builder("minicluster-messenger");
  builder.set_num_reactors(1);
  builder.connection_context_factory()->SetParentMemTracker(
      MemTracker::FindOrCreateTracker("minicluster"));
  RETURN_NOT_OK_PREPEND(builder.Build().MoveTo(&messenger_),
                        "Failed to start Messenger for minicluster");

  Status s = Env::Default()->CreateDir(data_root_);
  if (!s.ok() && !s.IsAlreadyPresent()) {
    RETURN_NOT_OK_PREPEND(s, "Could not create root dir " + data_root_);
  }

  LOG(INFO) << "Starting " << opts_.num_masters << " masters";
  RETURN_NOT_OK_PREPEND(StartMasters(), "Failed to start masters.");
  add_new_master_at_ = opts_.num_masters;

  LOG(INFO) << "Starting " << opts_.num_tablet_servers << " tablet servers";

  for (int i = 1; i <= opts_.num_tablet_servers; i++) {
    RETURN_NOT_OK_PREPEND(AddTabletServer(),
                          Substitute("Failed starting tablet server $0", i));
  }
  RETURN_NOT_OK(WaitForTabletServerCount(
                  opts_.num_tablet_servers,
                  MonoDelta::FromSeconds(kTabletServerRegistrationTimeoutSeconds)));

  return Status::OK();
}

void ExternalMiniCluster::Shutdown(NodeSelectionMode mode) {
  if (mode == ALL) {
    for (const scoped_refptr<ExternalMaster>& master : masters_) {
      if (master) {
        master->Shutdown();
      }
    }
  }

  for (const scoped_refptr<ExternalTabletServer>& ts : tablet_servers_) {
    ts->Shutdown();
  }
}

Status ExternalMiniCluster::Restart() {
  LOG(INFO) << "Restarting cluster with " << masters_.size() << " masters.";
  for (const scoped_refptr<ExternalMaster>& master : masters_) {
    if (master && master->IsShutdown()) {
      RETURN_NOT_OK_PREPEND(master->Restart(), "Cannot restart master bound at: " +
                                               master->bound_rpc_hostport().ToString());
    }
  }

  for (const scoped_refptr<ExternalTabletServer>& ts : tablet_servers_) {
    if (ts->IsShutdown()) {
      RETURN_NOT_OK_PREPEND(ts->Restart(), "Cannot restart tablet server bound at: " +
                                           ts->bound_rpc_hostport().ToString());
    }
  }

  RETURN_NOT_OK(WaitForTabletServerCount(
      tablet_servers_.size(),
      MonoDelta::FromSeconds(kTabletServerRegistrationTimeoutSeconds)));

  return Status::OK();
}

string ExternalMiniCluster::GetBinaryPath(const string& binary) const {
  CHECK(!daemon_bin_path_.empty());
  string default_path = JoinPathSegments(daemon_bin_path_, binary);
  if (Env::Default()->FileExists(default_path)) {
    return default_path;
  }

  // In CLion-based builds we sometimes have to look for the binary in other directories.
  string alternative_dir;
  if (binary == "yb-master") {
    alternative_dir = "master";
  } else if (binary == "yb-tserver") {
    alternative_dir = "tserver";
  } else {
    LOG(WARNING) << "Default path " << default_path << " for binary " << binary <<
      " does not exist, and no alternative directory is available for this binary";
    return default_path;
  }

  string alternative_path = JoinPathSegments(daemon_bin_path_,
    "../" + alternative_dir + "/" + binary);
  if (Env::Default()->FileExists(alternative_path)) {
    LOG(INFO) << "Default path " << default_path << " for binary " << binary <<
      " does not exist, using alternative location: " << alternative_path;
    return alternative_path;
  } else {
    LOG(WARNING) << "Neither " << default_path << " nor " << alternative_path << " exist";
    return default_path;
  }
}

string ExternalMiniCluster::GetDataPath(const string& daemon_id) const {
  CHECK(!data_root_.empty());
  return JoinPathSegments(data_root_, daemon_id);
}

namespace {
vector<string> SubstituteInFlags(const vector<string>& orig_flags,
                                 int index) {
  string str_index = strings::Substitute("$0", index);
  vector<string> ret;
  for (const string& orig : orig_flags) {
    ret.push_back(StringReplace(orig, "${index}", str_index, true));
  }
  return ret;
}

}  // anonymous namespace

void ExternalMiniCluster::StartShellMaster(ExternalMaster** new_master) {
  uint16_t rpc_port = AllocateFreePort();
  uint16_t http_port = AllocateFreePort();
  LOG(INFO) << "Using auto-assigned rpc_port " << rpc_port << "; http_port " << http_port
            << " to start a new external mini-cluster master.";

  string addr = Substitute("127.0.0.1:$0", rpc_port);

  string exe = GetBinaryPath(kMasterBinaryName);

  ExternalMaster* master = new ExternalMaster(
      add_new_master_at_,
      messenger_,
      exe,
      GetDataPath(Substitute("master-$0", add_new_master_at_)),
      opts_.extra_master_flags,
      addr,
      http_port,
      "");

  Status s = master->Start(true);

  if (!s.ok()) {
    LOG(FATAL) << Substitute("Unable to start 'shell' mode master at index $0, due to error $1.",
                             add_new_master_at_, s.ToString());
  }

  add_new_master_at_++;
  *new_master = master;
}

Status ExternalMiniClusterOptions::RemovePort(const uint16_t port) {
  auto iter = std::find(master_rpc_ports.begin(), master_rpc_ports.end(), port);

  if (iter == master_rpc_ports.end()) {
    return STATUS(InvalidArgument, Substitute(
        "Port to be removed '$0' not found in existing list of $1 masters.",
        port, num_masters));
  }

  master_rpc_ports.erase(iter);
  --num_masters;

  return Status::OK();
}

Status ExternalMiniClusterOptions::AddPort(const uint16_t port) {
  auto iter = std::find(master_rpc_ports.begin(), master_rpc_ports.end(), port);

  if (iter != master_rpc_ports.end()) {
    return STATUS(InvalidArgument, Substitute(
        "Port to be added '$0' already found in the existing list of $1 masters.",
        port, num_masters));
  }

  master_rpc_ports.push_back(port);
  ++num_masters;

  return Status::OK();
}

Status ExternalMiniCluster::CheckPortAndMasterSizes() const {
  if (opts_.num_masters != masters_.size() ||
      opts_.num_masters != opts_.master_rpc_ports.size()) {
    string fatal_err_msg = Substitute(
        "Mismatch number of masters in options $0, compared to masters vector $1 or rpc ports $2",
        opts_.num_masters, masters_.size(), opts_.master_rpc_ports.size());
    LOG(FATAL) << fatal_err_msg;
  }

  return Status::OK();
}

Status ExternalMiniCluster::AddMaster(ExternalMaster* master) {
  auto iter = std::find_if(masters_.begin(), masters_.end(), MasterComparator(master));

  if (iter != masters_.end()) {
    return STATUS(InvalidArgument, Substitute(
        "Master to be added '$0' already found in existing list of $1 masters.",
        master->bound_rpc_hostport().ToString(), opts_.num_masters));
  }

  RETURN_NOT_OK(opts_.AddPort(master->bound_rpc_hostport().port()));
  masters_.push_back(master);

  RETURN_NOT_OK(CheckPortAndMasterSizes());

  return Status::OK();
}

Status ExternalMiniCluster::RemoveMaster(ExternalMaster* master) {
  auto iter = std::find_if(masters_.begin(), masters_.end(), MasterComparator(master));

  if (iter == masters_.end()) {
    return STATUS(InvalidArgument, Substitute(
        "Master to be removed '$0' not found in existing list of $1 masters.",
        master->bound_rpc_hostport().ToString(), opts_.num_masters));
  }

  RETURN_NOT_OK(opts_.RemovePort(master->bound_rpc_hostport().port()));
  masters_.erase(iter);

  RETURN_NOT_OK(CheckPortAndMasterSizes());

  return Status::OK();
}

std::shared_ptr<ConsensusServiceProxy> ExternalMiniCluster::GetLeaderConsensusProxy() {
  auto leader_master_sock = GetLeaderMaster()->bound_rpc_addr();

  return std::make_shared<ConsensusServiceProxy>(messenger_, leader_master_sock);
}

std::shared_ptr<ConsensusServiceProxy> ExternalMiniCluster::GetConsensusProxy(
    scoped_refptr<ExternalMaster> master) {
  auto master_sock = master->bound_rpc_addr();

  return std::make_shared<ConsensusServiceProxy>(messenger_, master_sock);
}

Status ExternalMiniCluster::StepDownMasterLeader(TabletServerErrorPB::Code* error_code) {
  ExternalMaster* leader = GetLeaderMaster();
  string leader_uuid = leader->uuid();
  auto host_port = leader->bound_rpc_addr();
  LeaderStepDownRequestPB lsd_req;
  lsd_req.set_tablet_id(yb::master::kSysCatalogTabletId);
  lsd_req.set_dest_uuid(leader_uuid);
  LeaderStepDownResponsePB lsd_resp;
  RpcController lsd_rpc;
  lsd_rpc.set_timeout(opts_.timeout_);
  std::unique_ptr<ConsensusServiceProxy> proxy(new ConsensusServiceProxy(messenger_, host_port));
  RETURN_NOT_OK(proxy->LeaderStepDown(lsd_req, &lsd_resp, &lsd_rpc));
  if (lsd_resp.has_error()) {
    LOG(ERROR) << "LeaderStepDown for " << leader_uuid << " received error "
               << lsd_resp.error().ShortDebugString();
    *error_code = lsd_resp.error().code();
    return StatusFromPB(lsd_resp.error().status());
  }

  LOG(INFO) << "Leader at host/port '" << host_port << "' step down complete.";

  return Status::OK();
}

Status ExternalMiniCluster::StepDownMasterLeaderAndWaitForNewLeader() {
  ExternalMaster* leader = GetLeaderMaster();
  string old_leader_uuid = leader->uuid();
  string leader_uuid = old_leader_uuid;
  TabletServerErrorPB::Code error_code = TabletServerErrorPB::UNKNOWN_ERROR;
  LOG(INFO) << "Starting step down of leader " << leader->bound_rpc_addr();

  // while loop will not be needed once JIRA ENG-49 is fixed.
  int iter = 1;
  while (leader_uuid == old_leader_uuid) {
    Status s = StepDownMasterLeader(&error_code);
    // If step down hits any error except not-ready, exit.
    if (!s.ok() && error_code != TabletServerErrorPB::LEADER_NOT_READY_TO_STEP_DOWN) {
      return s;
    }
    sleep(3);  // TODO: add wait for election api.
    leader = GetLeaderMaster();
    leader_uuid = leader->uuid();
    LOG(INFO) << "Got new leader " << leader->bound_rpc_addr() << ", iter=" << iter;
    iter++;
  }

  return Status::OK();
}

Status ExternalMiniCluster::ChangeConfig(ExternalMaster* master,
                                         ChangeConfigType type,
                                         RaftPeerPB::MemberType member_type) {
  if (type != consensus::ADD_SERVER && type != consensus::REMOVE_SERVER) {
    return STATUS(InvalidArgument, Substitute("Invalid Change Config type $0", type));
  }

  ChangeConfigRequestPB req;
  ChangeConfigResponsePB resp;
  rpc::RpcController rpc;
  rpc.set_timeout(opts_.timeout_);

  RaftPeerPB peer_pb;
  peer_pb.set_permanent_uuid(master->uuid());
  if (type == consensus::ADD_SERVER) {
    peer_pb.set_member_type(member_type);
  }
  RETURN_NOT_OK(HostPortToPB(master->bound_rpc_hostport(), peer_pb.mutable_last_known_addr()));
  req.set_tablet_id(yb::master::kSysCatalogTabletId);
  req.set_type(type);
  *req.mutable_server() = peer_pb;

  // There could be timing window where we found the leader host/port, but an election in the
  // meantime could have made it step down. So we retry till we hit the leader correctly.
  int num_attempts = 1;
  while (true) {
    ExternalMaster* leader = GetLeaderMaster();
    std::unique_ptr<ConsensusServiceProxy>
      leader_proxy(new ConsensusServiceProxy(messenger_, leader->bound_rpc_addr()));
    string leader_uuid = leader->uuid();

    if (type == consensus::REMOVE_SERVER && leader_uuid == req.server().permanent_uuid()) {
      RETURN_NOT_OK(StepDownMasterLeaderAndWaitForNewLeader());
      leader = GetLeaderMaster();
      leader_uuid = leader->uuid();
      leader_proxy.reset(new ConsensusServiceProxy(messenger_, leader->bound_rpc_addr()));
    }

    req.set_dest_uuid(leader_uuid);
    RETURN_NOT_OK(leader_proxy->ChangeConfig(req, &resp, &rpc));
    if (resp.has_error()) {
      if (resp.error().code() != TabletServerErrorPB::NOT_THE_LEADER &&
          resp.error().code() != TabletServerErrorPB::LEADER_NOT_READY_CHANGE_CONFIG) {
        return STATUS(RuntimeError, Substitute("Change Config RPC to leader hit error: $0",
                                               resp.error().ShortDebugString()));
      }
    } else {
      break;
    }

    // Need to retry as we come here with NOT_THE_LEADER.
    if (num_attempts >= kMaxRetryIterations) {
      return STATUS(IllegalState,
                    Substitute("Failed to complete ChangeConfig request '$0' even after maximum "
                               "number of attempts. Last error '$1'",
                               req.ShortDebugString(), resp.error().ShortDebugString()));
    }

    SleepFor(MonoDelta::FromSeconds(1));

    LOG(INFO) << "Resp error '" << resp.error().ShortDebugString() << "', num=" << num_attempts
              << ", retrying...";

    rpc.Reset();
    num_attempts++;
  }

  LOG(INFO) << "Master " << master->bound_rpc_hostport().ToString() << ", change type "
            << type << " to " << masters_.size() << " masters.";

  if (type == consensus::ADD_SERVER) {
    return AddMaster(master);
  } else if (type == consensus::REMOVE_SERVER) {
    return RemoveMaster(master);
  }

  string err_msg = Substitute("Should not reach here - change type $0", type);

  LOG(FATAL) << err_msg;

  // Satisfy the compiler with a return from here
  return STATUS(RuntimeError, err_msg);
}

// We look for the exact master match. Since it is possible to stop/restart master on
// a given host/port, we do not want a stale master pointer input to match a newer master.
int ExternalMiniCluster::GetIndexOfMaster(ExternalMaster* master) const {
  for (int i = 0; i < masters_.size(); i++) {
    if (masters_[i].get() == master) {
      return i;
    }
  }
  return -1;
}

Status ExternalMiniCluster::GetNumMastersAsSeenBy(ExternalMaster* master, int* num_peers) {
  ListMastersRequestPB list_req;
  ListMastersResponsePB list_resp;
  int index = GetIndexOfMaster(master);

  if (index == -1) {
    return STATUS(InvalidArgument, Substitute(
        "Given master '$0' not in the current list of $1 masters.",
        master->bound_rpc_hostport().ToString(), masters_.size()));
  }

  std::shared_ptr<MasterServiceProxy> proxy = master_proxy(index);
  rpc::RpcController rpc;
  rpc.set_timeout(opts_.timeout_);
  RETURN_NOT_OK(proxy->ListMasters(list_req, &list_resp, &rpc));
  if (list_resp.has_error()) {
    return STATUS(RuntimeError, Substitute(
        "List Masters RPC response hit error: $0", list_resp.error().ShortDebugString()));
  }

  LOG(INFO) << "List Masters for master at index " << index
            << " got " << list_resp.masters_size() << " peers";

  *num_peers = list_resp.masters_size();

  return Status::OK();
}

Status ExternalMiniCluster::WaitForLeaderCommitTermAdvance() {
  consensus::OpId start_opid;
  RETURN_NOT_OK(GetLastOpIdForLeader(&start_opid));
  LOG(INFO) << "Start OPID : " << start_opid.ShortDebugString();

  // Need not do any wait if it is a restart case - so the commit term will be > 0.
  if (start_opid.term() != 0)
    return Status::OK();

  MonoTime now = MonoTime::Now();
  MonoTime deadline = now;
  deadline.AddDelta(opts_.timeout_);
  auto opid = start_opid;

  for (int i = 1; now.ComesBefore(deadline); ++i) {
    if (opid.term() > start_opid.term()) {
      LOG(INFO) << "Final OPID: " << opid.ShortDebugString() << " after "
                << i << " iterations.";

      return Status::OK();
    }
    SleepFor(MonoDelta::FromMilliseconds(min(i, 10)));
    RETURN_NOT_OK(GetLastOpIdForLeader(&opid));
    now = MonoTime::Now();
  }

  return STATUS(TimedOut, Substitute("Term did not advance from $0.", start_opid.term()));
}

Status ExternalMiniCluster::GetLastOpIdForEachMasterPeer(
    const MonoDelta& timeout,
    consensus::OpIdType opid_type,
    vector<consensus::OpId>* op_ids) {
  GetLastOpIdRequestPB opid_req;
  GetLastOpIdResponsePB opid_resp;
  opid_req.set_tablet_id(yb::master::kSysCatalogTabletId);
  RpcController controller;
  controller.set_timeout(timeout);

  op_ids->clear();
  for (scoped_refptr<ExternalMaster> master : masters_) {
    opid_req.set_dest_uuid(master->uuid());
    opid_req.set_opid_type(opid_type);
    RETURN_NOT_OK_PREPEND(
        GetConsensusProxy(master)->GetLastOpId(opid_req, &opid_resp, &controller),
        Substitute("Failed to fetch last op id from $0", master->bound_rpc_hostport().port()));
    op_ids->push_back(opid_resp.opid());
    controller.Reset();
  }

  return Status::OK();
}

Status ExternalMiniCluster::WaitForMastersToCommitUpTo(int target_index) {
  auto deadline = CoarseMonoClock::Now() + opts_.timeout_.ToSteadyDuration();

  for (int i = 1; CoarseMonoClock::Now() < deadline; i++) {
    vector<consensus::OpId> ids;
    Status s = GetLastOpIdForEachMasterPeer(opts_.timeout_, consensus::COMMITTED_OPID, &ids);

    if (s.ok()) {
      bool any_behind = false;
      for (const auto& id : ids) {
        if (id.index() < target_index) {
          any_behind = true;
          break;
        }
      }
      if (!any_behind) {
        LOG(INFO) << "Committed up to " << target_index;
        return Status::OK();
      }
    } else {
      LOG(WARNING) << "Got error getting last opid for each replica: " << s.ToString();
    }

    SleepFor(MonoDelta::FromMilliseconds(min(i * 100, 1000)));
  }

  return STATUS_FORMAT(TimedOut,
                       "Index $0 not available on all replicas after $1. ",
                       target_index,
                       opts_.timeout_);
}

Status ExternalMiniCluster::GetIsMasterLeaderServiceReady(ExternalMaster* master) {
  IsMasterLeaderReadyRequestPB req;
  IsMasterLeaderReadyResponsePB resp;
  int index = GetIndexOfMaster(master);

  if (index == -1) {
    return STATUS(InvalidArgument, Substitute(
        "Given master '$0' not in the current list of $1 masters.",
        master->bound_rpc_hostport().ToString(), masters_.size()));
  }

  std::shared_ptr<MasterServiceProxy> proxy = master_proxy(index);
  rpc::RpcController rpc;
  rpc.set_timeout(opts_.timeout_);
  RETURN_NOT_OK(proxy->IsMasterLeaderServiceReady(req, &resp, &rpc));
  if (resp.has_error()) {
    return STATUS(RuntimeError, Substitute(
        "Is master ready RPC response hit error: $0", resp.error().ShortDebugString()));
  }

  return Status::OK();
}

Status ExternalMiniCluster::GetLastOpIdForLeader(consensus::OpId* opid) {
  ExternalMaster* leader = GetLeaderMaster();
  auto leader_master_sock = leader->bound_rpc_addr();
  std::shared_ptr<ConsensusServiceProxy> leader_proxy =
    std::make_shared<ConsensusServiceProxy>(messenger_, leader_master_sock);

  RETURN_NOT_OK(itest::GetLastOpIdForMasterReplica(
      leader_proxy,
      yb::master::kSysCatalogTabletId,
      leader->uuid(),
      consensus::COMMITTED_OPID,
      opts_.timeout_,
      opid));

  return Status::OK();
}

Status ExternalMiniCluster::StartMasters() {
  int num_masters = opts_.num_masters;

  if (opts_.master_rpc_ports.size() != num_masters) {
    LOG(FATAL) << num_masters << " masters requested, but " <<
        opts_.master_rpc_ports.size() << " ports specified in 'master_rpc_ports'";
  }

  for (int i = 0; i < opts_.master_rpc_ports.size(); ++i) {
    if (opts_.master_rpc_ports[i] == 0) {
      opts_.master_rpc_ports[i] = AllocateFreePort();
      LOG(INFO) << "Using an auto-assigned port " << opts_.master_rpc_ports[i]
        << " to start an external mini-cluster master";
    }
  }

  vector<string> peer_addrs;
  for (int i = 0; i < num_masters; i++) {
    string addr = Substitute("127.0.0.1:$0", opts_.master_rpc_ports[i]);
    peer_addrs.push_back(addr);
  }
  string peer_addrs_str = JoinStrings(peer_addrs, ",");
  vector<string> flags = opts_.extra_master_flags;
  flags.push_back("--enable_leader_failure_detection=true");
  string exe = GetBinaryPath(kMasterBinaryName);

  // Start the masters.
  for (int i = 0; i < num_masters; i++) {
    uint16_t http_port = AllocateFreePort();
    scoped_refptr<ExternalMaster> peer =
      new ExternalMaster(
        i,
        messenger_,
        exe,
        GetDataPath(Substitute("master-$0", i)),
        SubstituteInFlags(flags, i),
        peer_addrs[i],
        http_port,
        peer_addrs_str);
    RETURN_NOT_OK_PREPEND(peer->Start(),
                          Substitute("Unable to start Master at index $0", i));
    masters_.push_back(peer);
  }

  return Status::OK();
}

string ExternalMiniCluster::GetBindIpForTabletServer(int index) const {
  if (opts_.bind_to_unique_loopback_addresses) {
    pid_t p = getpid();
    CHECK_LE(p, MathLimits<uint16_t>::kMax)
        << "bind_to_unique_loopback_addresses does not work on systems with >16-bit pid";
    return Substitute("127.$0.$1.$2", p >> 8, p & 0xff, index);
  } else {
    return "127.0.0.1";
  }
}

Status ExternalMiniCluster::AddTabletServer() {
  CHECK(GetLeaderMaster() != nullptr)
      << "Must have started at least 1 master before adding tablet servers";

  int idx = tablet_servers_.size();

  string exe = GetBinaryPath(kTabletServerBinaryName);
  vector<HostPort> master_hostports;
  for (int i = 0; i < num_masters(); i++) {
    master_hostports.push_back(DCHECK_NOTNULL(master(i))->bound_rpc_hostport());
  }

  const uint16_t ts_rpc_port = AllocateFreePort();
  const uint16_t ts_http_port = AllocateFreePort();
  const uint16_t redis_rpc_port = AllocateFreePort();
  const uint16_t redis_http_port = AllocateFreePort();
  const uint16_t cql_rpc_port = AllocateFreePort();
  const uint16_t cql_http_port = AllocateFreePort();
  const uint16_t pgsql_rpc_port = AllocateFreePort();
  const uint16_t pgsql_http_port = AllocateFreePort();
  scoped_refptr<ExternalTabletServer> ts = new ExternalTabletServer(
      idx, messenger_, exe, GetDataPath(Substitute("ts-$0", idx)), GetBindIpForTabletServer(idx),
      ts_rpc_port, ts_http_port, redis_rpc_port, redis_http_port,
      cql_rpc_port, cql_http_port,
      pgsql_rpc_port, pgsql_http_port,
      master_hostports, SubstituteInFlags(opts_.extra_tserver_flags, idx));
  RETURN_NOT_OK(ts->Start());
  tablet_servers_.push_back(ts);
  return Status::OK();
}

Status ExternalMiniCluster::WaitForTabletServerCount(int count, const MonoDelta& timeout) {
  MonoTime deadline = MonoTime::Now();
  deadline.AddDelta(timeout);

  while (true) {
    MonoDelta remaining = deadline.GetDeltaSince(MonoTime::Now());
    if (remaining.ToSeconds() < 0) {
      return STATUS(TimedOut, Substitute("$0 TS(s) never registered with master", count));
    }

    for (int i = 0; i < masters_.size(); i++) {
      master::ListTabletServersRequestPB req;
      master::ListTabletServersResponsePB resp;
      rpc::RpcController rpc;
      rpc.set_timeout(remaining);
      RETURN_NOT_OK_PREPEND(master_proxy(i)->ListTabletServers(req, &resp, &rpc),
                            "ListTabletServers RPC failed");
      // ListTabletServers() may return servers that are no longer online.
      // Do a second step of verification to verify that the descs that we got
      // are aligned (same uuid/seqno) with the TSs that we have in the cluster.
      int match_count = 0;
      for (const master::ListTabletServersResponsePB_Entry& e : resp.servers()) {
        for (const scoped_refptr<ExternalTabletServer>& ets : tablet_servers_) {
          if (ets->instance_id().permanent_uuid() == e.instance_id().permanent_uuid() &&
              ets->instance_id().instance_seqno() == e.instance_id().instance_seqno()) {
            match_count++;
            break;
          }
        }
      }
      if (match_count == count) {
        LOG(INFO) << count << " TS(s) registered with Master";
        return Status::OK();
      }
    }
    SleepFor(MonoDelta::FromMilliseconds(1));
  }
}

void ExternalMiniCluster::AssertNoCrashes() {
  vector<ExternalDaemon*> daemons = this->daemons();
  for (ExternalDaemon* d : daemons) {
    if (d->IsShutdown()) continue;
    EXPECT_TRUE(d->IsProcessAlive()) << "At least one process crashed";
  }
}

Status ExternalMiniCluster::WaitForTabletsRunning(ExternalTabletServer* ts,
                                                  const MonoDelta& timeout) {
  TabletServerServiceProxy proxy(messenger_, ts->bound_rpc_addr());
  ListTabletsRequestPB req;
  ListTabletsResponsePB resp;

  MonoTime deadline = MonoTime::Now();
  deadline.AddDelta(timeout);
  while (MonoTime::Now().ComesBefore(deadline)) {
    rpc::RpcController rpc;
    rpc.set_timeout(MonoDelta::FromSeconds(10));
    RETURN_NOT_OK(proxy.ListTablets(req, &resp, &rpc));
    if (resp.has_error()) {
      return StatusFromPB(resp.error().status());
    }

    int num_not_running = 0;
    for (const StatusAndSchemaPB& status : resp.status_and_schema()) {
      if (status.tablet_status().state() != tablet::RUNNING) {
        num_not_running++;
      }
    }

    if (num_not_running == 0) {
      return Status::OK();
    }

    SleepFor(MonoDelta::FromMilliseconds(10));
  }

  return STATUS(TimedOut, resp.DebugString());
}

Status ExternalMiniCluster::WaitForTSToCrash(int index, const MonoDelta& timeout) {
  ExternalTabletServer* ts = tablet_server(index);
  return WaitForTSToCrash(ts, timeout);
}

Status ExternalMiniCluster::WaitForTSToCrash(const ExternalTabletServer* ts,
                                             const MonoDelta& timeout) {
  MonoTime deadline = MonoTime::Now();
  deadline.AddDelta(timeout);
  while (MonoTime::Now().ComesBefore(deadline)) {
    if (!ts->IsProcessAlive()) {
      return Status::OK();
    }
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  return STATUS(TimedOut, Substitute("TS $0 did not crash!", ts->instance_id().permanent_uuid()));
}

namespace {
void LeaderMasterCallback(HostPort* dst_hostport,
                          Synchronizer* sync,
                          const Status& status,
                          const HostPort& result) {
  if (status.ok()) {
    *dst_hostport = result;
  }
  sync->StatusCB(status);
}
}  // anonymous namespace

Status ExternalMiniCluster::GetFirstNonLeaderMasterIndex(int* idx) {
  return GetPeerMasterIndex(idx, false);
}

Status ExternalMiniCluster::GetLeaderMasterIndex(int* idx) {
  return GetPeerMasterIndex(idx, true);
}

Status ExternalMiniCluster::GetPeerMasterIndex(int* idx, bool is_leader) {
  Synchronizer sync;
  std::vector<Endpoint> addrs;
  HostPort leader_master_hp;
  MonoTime deadline = MonoTime::Now();
  deadline.AddDelta(MonoDelta::FromSeconds(5));

  *idx = 0;  // default to 0'th index, even in case of errors.

  for (const scoped_refptr<ExternalMaster>& master : masters_) {
    addrs.push_back(master->bound_rpc_addr());
  }
  rpc::Rpcs rpcs;
  auto rpc = rpc::StartRpc<GetLeaderMasterRpc>(
      Bind(&LeaderMasterCallback, &leader_master_hp, &sync),
      addrs,
      deadline,
      messenger_,
      &rpcs);
  RETURN_NOT_OK(sync.Wait());
  rpcs.Shutdown();
  bool found = false;
  for (int i = 0; i < masters_.size(); i++) {
    if (is_leader && masters_[i]->bound_rpc_hostport().port() == leader_master_hp.port() ||
        !is_leader && masters_[i]->bound_rpc_hostport().port() != leader_master_hp.port()) {
      found = true;
      *idx = i;
      break;
    }
  }

  const string peer_type = is_leader ? "leader" : "non-leader";
  if (!found) {
    // There is never a situation where this should happen, so it's
    // better to exit with a FATAL log message right away vs. return a
    // Status::IllegalState().
    LOG(FATAL) << "Peer " << peer_type << " master is not in masters_ list.";
  }

  LOG(INFO) << "Found peer " << peer_type << " at index " << *idx << ".";

  return Status::OK();
}

ExternalMaster* ExternalMiniCluster::GetLeaderMaster() {
  int idx = 0;
  int num_attempts = 0;
  Status s;
  // Retry to get the leader master's index - due to timing issues (like election in progress).
  do {
    ++num_attempts;
    s = GetLeaderMasterIndex(&idx);
    if (!s.ok()) {
      LOG(INFO) << "GetLeaderMasterIndex@" << num_attempts << " hit error: " << s.ToString();
      if (num_attempts >= kMaxRetryIterations) {
        LOG(WARNING) << "Failed to get leader master after " << num_attempts << " attempts, "
                     << "returning the first master.";
        break;
      }
      SleepFor(MonoDelta::FromMilliseconds(num_attempts * 10));
    }
  } while (!s.ok());

  return master(idx);
}

ExternalTabletServer* ExternalMiniCluster::tablet_server_by_uuid(const std::string& uuid) const {
  for (const scoped_refptr<ExternalTabletServer>& ts : tablet_servers_) {
    if (ts->instance_id().permanent_uuid() == uuid) {
      return ts.get();
    }
  }
  return nullptr;
}

int ExternalMiniCluster::tablet_server_index_by_uuid(const std::string& uuid) const {
  for (int i = 0; i < tablet_servers_.size(); i++) {
    if (tablet_servers_[i]->uuid() == uuid) {
      return i;
    }
  }
  return -1;
}

vector<ExternalDaemon*> ExternalMiniCluster::master_daemons() const {
  vector<ExternalDaemon*> results;
  for (const scoped_refptr<ExternalMaster>& master : masters_) {
    results.push_back(master.get());
  }
  return results;
}

vector<ExternalDaemon*> ExternalMiniCluster::daemons() const {
  vector<ExternalDaemon*> results;
  for (const scoped_refptr<ExternalTabletServer>& ts : tablet_servers_) {
    results.push_back(ts.get());
  }
  for (const scoped_refptr<ExternalMaster>& master : masters_) {
    results.push_back(master.get());
  }
  return results;
}

std::shared_ptr<rpc::Messenger> ExternalMiniCluster::messenger() {
  return messenger_;
}

std::shared_ptr<MasterServiceProxy> ExternalMiniCluster::master_proxy() {
  CHECK_EQ(masters_.size(), 1);
  return master_proxy(0);
}

std::shared_ptr<MasterServiceProxy> ExternalMiniCluster::master_proxy(int idx) {
  CHECK_LT(idx, masters_.size());
  return std::make_shared<MasterServiceProxy>(
      messenger_, CHECK_NOTNULL(master(idx))->bound_rpc_addr());
}

Status ExternalMiniCluster::DoCreateClient(client::YBClientBuilder* builder,
                                           std::shared_ptr<client::YBClient>* client) {
  CHECK(!masters_.empty());
  builder->clear_master_server_addrs();
  for (const scoped_refptr<ExternalMaster>& master : masters_) {
    builder->add_master_server_addr(master->bound_rpc_hostport().ToString());
  }
  return builder->Build(client);
}

Endpoint ExternalMiniCluster::DoGetLeaderMasterBoundRpcAddr() {
  return GetLeaderMaster()->bound_rpc_addr();
}

Status ExternalMiniCluster::SetFlag(ExternalDaemon* daemon,
                                    const string& flag,
                                    const string& value) {
  server::GenericServiceProxy proxy(messenger_, daemon->bound_rpc_addr());

  rpc::RpcController controller;
  controller.set_timeout(MonoDelta::FromSeconds(30));
  server::SetFlagRequestPB req;
  server::SetFlagResponsePB resp;
  req.set_flag(flag);
  req.set_value(value);
  req.set_force(true);
  RETURN_NOT_OK_PREPEND(proxy.SetFlag(req, &resp, &controller),
                        "rpc failed");
  if (resp.result() != server::SetFlagResponsePB::SUCCESS) {
    return STATUS(RemoteError, "failed to set flag",
                               resp.ShortDebugString());
  }
  return Status::OK();
}

Status ExternalMiniCluster::SetFlagOnTServers(const string& flag, const string& value) {
  for (const auto& tablet_server : tablet_servers_) {
    RETURN_NOT_OK(SetFlag(tablet_server.get(), flag, value));
  }
  return Status::OK();
}


uint16_t ExternalMiniCluster::AllocateFreePort() {
  // This will take a file lock ensuring the port does not get claimed by another thread/process
  // and add it to our vector of such locks that will be freed on minicluster shutdown.
  free_port_file_locks_.emplace_back();
  return GetFreePort(&free_port_file_locks_.back());
}

Status ExternalMiniCluster::StartElection(ExternalMaster* master) {
  auto master_sock = master->bound_rpc_addr();
  std::shared_ptr<ConsensusServiceProxy> master_proxy =
    std::make_shared<ConsensusServiceProxy>(messenger_, master_sock);

  RunLeaderElectionRequestPB req;
  req.set_dest_uuid(master->uuid());
  req.set_tablet_id(yb::master::kSysCatalogTabletId);
  RunLeaderElectionResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(opts_.timeout_);
  RETURN_NOT_OK(master_proxy->RunLeaderElection(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status())
               .CloneAndPrepend(Substitute("Code $0",
                                           TabletServerErrorPB::Code_Name(resp.error().code())));
  }
  return Status::OK();
}

//------------------------------------------------------------
// ExternalDaemon
//------------------------------------------------------------

namespace {

// Global state to manage all log tailer threads. This state is managed using Singleton from gutil
// and is never deallocated.
struct GlobalLogTailerState {
  mutex logging_mutex;
  atomic<int> next_log_tailer_id;

  // We need some references to these heap-allocated atomic booleans so that ASAN would not consider
  // them memory leaks.
  mutex id_to_stopped_flag_mutex;
  map<int, atomic<bool>*> id_to_stopped_flag;

  GlobalLogTailerState() {
    next_log_tailer_id.store(0);
  }
};

}  // anonymous namespace

class ExternalDaemon::LogTailerThread {
 public:
  LogTailerThread(const string line_prefix,
                  const int child_fd,
                  ostream* const out)
      : id_(global_state()->next_log_tailer_id.fetch_add(1)),
        stopped_(CreateStoppedFlagForId(id_)),
        thread_desc_(Substitute("log tailer thread for prefix $0", line_prefix)),
        thread_([=] {
          VLOG(1) << "Starting " << thread_desc_;
          FILE* const fp = fdopen(child_fd, "rb");
          char buf[65536];
          const atomic<bool>* stopped;

          {
            lock_guard<mutex> l(state_lock_);
            stopped = stopped_;
          }

          // Instead of doing a nonblocking read, we detach this thread and allow it to block
          // indefinitely trying to read from a child process's stream where nothing is happening.
          // This is probably OK as long as we are careful to avoid accessing any state that might
          // have been already destructed (e.g. logging, cout/cerr, member fields of this class,
          // etc.) in case we do get unblocked. Instead, we keep a local pointer to the atomic
          // "stopped" flag, and that allows us to safely check if it is OK to print log messages.
          // The "stopped" flag itself is never deallocated.
          bool is_eof = false;
          bool is_fgets_null = false;
          while (!(is_eof = feof(fp)) &&
                 !(is_fgets_null = (fgets(buf, sizeof(buf), fp) == nullptr)) &&
                 !stopped->load()) {
            size_t l = strlen(buf);
            const char* maybe_end_of_line = l > 0 && buf[l - 1] == '\n' ? "" : "\n";
            // Synchronize tailing output from all external daemons for simplicity.
            lock_guard<mutex> lock(global_state()->logging_mutex);
            if (stopped->load()) break;
            // Make sure we always output an end-of-line character.
            *out << line_prefix << " " << buf << maybe_end_of_line;
          }
          fclose(fp);
          if (!stopped->load()) {
            // It might not be safe to log anything if we have already stopped.
            VLOG(1) << "Exiting " << thread_desc_
                    << ": is_eof=" << is_eof
                    << ", is_fgets_null=" << is_fgets_null
                    << ", stopped=0";
          }
        }) {
    thread_.detach();
  }

  ~LogTailerThread() {
    VLOG(1) << "Stopping " << thread_desc_;
    lock_guard<mutex> l(state_lock_);
    stopped_->store(true);
  }

 private:
  static GlobalLogTailerState* global_state() {
    return Singleton<GlobalLogTailerState>::get();
  }

  static atomic<bool>* CreateStoppedFlagForId(int id) {
    lock_guard<mutex> lock(global_state()->id_to_stopped_flag_mutex);
    // This is never deallocated, but we add this pointer to the id_to_stopped_flag map referenced
    // from the global state singleton, and that apparently makes ASAN no longer consider this to be
    // a memory leak. We don't need to check if the id already exists in the map, because this
    // function is never invoked with a particular id more than once.
    auto* const stopped = new atomic<bool>();
    stopped->store(false);
    global_state()->id_to_stopped_flag[id] = stopped;
    return stopped;
  }

  const int id_;

  // This lock protects the stopped_ pointer in case of a race between tailer thread's
  // initialization (i.e. before it gets into its loop) and the destructor.
  mutex state_lock_;

  atomic<bool>* const stopped_;
  const string thread_desc_;  // A human-readable description of this thread.
  thread thread_;
};

ExternalDaemon::ExternalDaemon(
    std::string daemon_id,
    std::shared_ptr<rpc::Messenger> messenger,
    string exe,
    string data_dir,
    string server_type,
    vector<string> extra_flags)
  : daemon_id_(daemon_id),
    messenger_(std::move(messenger)),
    exe_(std::move(exe)),
    data_dir_(std::move(data_dir)),
    full_data_dir_(GetServerTypeDataPath(data_dir_, std::move(server_type))),
    extra_flags_(std::move(extra_flags)) {}

ExternalDaemon::~ExternalDaemon() {
}

bool ExternalDaemon::ServerInfoPathsExist() {
  return Env::Default()->FileExists(GetServerInfoPath());
}

Status ExternalDaemon::BuildServerStateFromInfoPath() {
  return BuildServerStateFromInfoPath(GetServerInfoPath(), &status_);
}

Status ExternalDaemon::BuildServerStateFromInfoPath(
    const string& info_path, std::unique_ptr<ServerStatusPB>* server_status) {
  server_status->reset(new ServerStatusPB());
  RETURN_NOT_OK_PREPEND(pb_util::ReadPBFromPath(Env::Default(), info_path, (*server_status).get()),
                        "Failed to read info file from " + info_path);
  return Status::OK();
}

string ExternalDaemon::GetServerInfoPath() {
  return JoinPathSegments(full_data_dir_, "info.pb");
}

Status ExternalDaemon::DeleteServerInfoPaths() {
  return Env::Default()->DeleteFile(GetServerInfoPath());
}

Status ExternalDaemon::StartProcess(const vector<string>& user_flags) {
  CHECK(!process_);

  vector<string> argv;
  // First the exe for argv[0]
  argv.push_back(BaseName(exe_));

  // Then all the flags coming from the minicluster framework.
  argv.insert(argv.end(), user_flags.begin(), user_flags.end());

  // Disable callhome.
  argv.push_back("--callhome_enabled=false");

  // Enable metrics logging.
  // Even though we set -logtostderr down below, metrics logs end up being written
  // based on -log_dir. So, we have to set that too.
  argv.push_back("--metrics_log_interval_ms=1000");
  argv.push_back("--log_dir=" + full_data_dir_);

  // Then the "extra flags" passed into the ctor (from the ExternalMiniCluster
  // options struct). These come at the end so they can override things like
  // web port or RPC bind address if necessary.
  argv.insert(argv.end(), extra_flags_.begin(), extra_flags_.end());

  // Tell the server to dump its port information so we can pick it up.
  const string info_path = GetServerInfoPath();
  argv.push_back("--server_dump_info_path=" + info_path);
  argv.push_back("--server_dump_info_format=pb");

  // We use ephemeral ports in many tests. They don't work for production, but are OK
  // in unit tests.
  argv.push_back("--rpc_server_allow_ephemeral_ports");

  // A previous instance of the daemon may have run in the same directory. So, remove
  // the previous info file if it's there.
  Status s = DeleteServerInfoPaths();
  if (!s.ok()) {
    LOG (WARNING) << "Failed to delete info paths: " << s.ToString();
  }

  // Ensure that logging goes to the test output and doesn't get buffered.
  argv.push_back("--logtostderr");
  argv.push_back("--logbuflevel=-1");

  // Use the same verbose logging level in the child process as in the test driver.
  if (FLAGS_v != 0) {  // Skip this option if it has its default value (0).
    argv.push_back(Substitute("-v=$0", FLAGS_v));
  }
  if (!FLAGS_vmodule.empty()) {
    argv.push_back(Substitute("--vmodule=$0", FLAGS_vmodule));
  }
  if (FLAGS_mem_tracker_logging) {
    argv.push_back("--mem_tracker_logging");
  }
  if (FLAGS_mem_tracker_log_stack_trace) {
    argv.push_back("--mem_tracker_log_stack_trace");
  }

  const char* test_invocation_id = getenv("YB_TEST_INVOCATION_ID");
  if (test_invocation_id) {
    // We use --metric_node_name=... to include a unique "test invocation id" into the command
    // line so we can kill any stray processes later. --metric_node_name is normally how we pass
    // the Universe ID to the cluster. We could use any other flag that is present in yb-master
    // and yb-tserver for this.
    argv.push_back(Format("--metric_node_name=$0", test_invocation_id));
  }

  string fatal_details_path_prefix = GetFatalDetailsPathPrefix();
  argv.push_back(Format(
      "--fatal_details_path_prefix=$0.$1", GetFatalDetailsPathPrefix(), daemon_id_));

  argv.push_back(Format("--minicluster_daemon_id=$0", daemon_id_));

  gscoped_ptr<Subprocess> p(new Subprocess(exe_, argv));
  p->ShareParentStdout(false);
  p->ShareParentStderr(false);
  auto default_output_prefix = Substitute("[$0]", daemon_id_);
  LOG(INFO) << "Running " << default_output_prefix << ": " << exe_ << "\n"
    << JoinStrings(argv, "\n");
  if (!FLAGS_external_daemon_heap_profile_prefix.empty()) {
    p->SetEnv("HEAPPROFILE",
              FLAGS_external_daemon_heap_profile_prefix + "_" + daemon_id_);
    p->SetEnv("HEAPPROFILESIGNAL", std::to_string(kHeapProfileSignal));
  }

  RETURN_NOT_OK_PREPEND(p->Start(),
                        Substitute("Failed to start subprocess $0", exe_));

  stdout_tailer_thread_ = unique_ptr<LogTailerThread>(new LogTailerThread(
      Substitute("[$0 stdout]", daemon_id_), p->ReleaseChildStdoutFd(), &std::cout));

  // We will mostly see stderr output from the child process (because of --logtostderr), so we'll
  // assume that by default in the output prefix.
  stderr_tailer_thread_ = unique_ptr<LogTailerThread>(new LogTailerThread(
      default_output_prefix, p->ReleaseChildStderrFd(), &std::cerr));

  // The process is now starting -- wait for the bound port info to show up.
  Stopwatch sw;
  sw.start();
  bool success = false;
  while (sw.elapsed().wall_seconds() < kProcessStartTimeoutSeconds) {
    if (ServerInfoPathsExist()) {
      success = true;
      break;
    }
    SleepFor(MonoDelta::FromMilliseconds(10));
    int rc;
    Status s = p->WaitNoBlock(&rc);
    if (s.IsTimedOut()) {
      // The process is still running.
      continue;
    }
    RETURN_NOT_OK_PREPEND(s, Substitute("Failed waiting on $0", exe_));
    return STATUS(RuntimeError,
      Substitute("Process exited with rc=$0", rc),
      exe_);
  }

  if (!success) {
    ignore_result(p->Kill(SIGKILL));
    return STATUS(TimedOut,
        Substitute("Timed out after $0s waiting for process ($1) to write info file ($2)",
                   kProcessStartTimeoutSeconds, exe_, info_path));
  }

  RETURN_NOT_OK(BuildServerStateFromInfoPath());
  LOG(INFO) << "Started " << default_output_prefix << " " << exe_ << " as pid " << p->pid();
  VLOG(1) << exe_ << " instance information:\n" << status_->DebugString();

  process_.swap(p);
  return Status::OK();
}

Status ExternalDaemon::Pause() {
  if (!process_) return Status::OK();
  VLOG(1) << "Pausing " << ProcessNameAndPidStr();
  return process_->Kill(SIGSTOP);
}

Status ExternalDaemon::Resume() {
  if (!process_) return Status::OK();
  VLOG(1) << "Resuming " << ProcessNameAndPidStr();
  return process_->Kill(SIGCONT);
}

bool ExternalDaemon::IsShutdown() const {
  return process_.get() == nullptr;
}

bool ExternalDaemon::IsProcessAlive() const {
  if (IsShutdown()) {
    return false;
  }

  int rc = 0;
  Status s = process_->WaitNoBlock(&rc);
  // If the non-blocking Wait "times out", that means the process
  // is running.
  return s.IsTimedOut();
}

pid_t ExternalDaemon::pid() const {
  return process_->pid();
}

void ExternalDaemon::Shutdown() {
  if (!process_) return;

  LOG_WITH_PREFIX(INFO) << "Starting Shutdown()";

  // Before we kill the process, store the addresses. If we're told to start again we'll reuse
  // these.
  bound_rpc_ = bound_rpc_hostport();
  bound_http_ = bound_http_hostport();

  if (IsProcessAlive()) {
    // In coverage builds, ask the process nicely to flush coverage info
    // before we kill -9 it. Otherwise, we never get any coverage from
    // external clusters.
    FlushCoverage();

    if (!FLAGS_external_daemon_heap_profile_prefix.empty()) {
      // The child process has been configured using the HEAPPROFILESIGNAL environment variable to
      // create a heap profile on receiving kHeapProfileSignal.
      static const int kWaitMs = 100;
      LOG(INFO) << "Sending signal " << kHeapProfileSignal << " to " << ProcessNameAndPidStr()
                << " to capture a heap profile. Waiting for " << kWaitMs << " ms afterwards.";
      ignore_result(process_->Kill(kHeapProfileSignal));
      std::this_thread::sleep_for(std::chrono::milliseconds(kWaitMs));
    }

    if (FLAGS_external_daemon_safe_shutdown) {
      // We put 'SIGTERM' in quotes because an unquoted one would be treated as a test failure
      // by our regular expressions in common-test-env.sh.
      LOG(INFO) << "Terminating " << ProcessNameAndPidStr() << " using 'SIGTERM' signal";
      ignore_result(process_->Kill(SIGTERM));
      int total_delay_ms = 0;
      int current_delay_ms = 10;
      for (int i = 0; i < 10 && IsProcessAlive(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(current_delay_ms));
        total_delay_ms += current_delay_ms;
        current_delay_ms += 10;  // will sleep for 10ms, then 20ms, etc.
      }

      if (IsProcessAlive()) {
        LOG(INFO) << "The process " << ProcessNameAndPidStr() << " is still running after "
                  << total_delay_ms << " ms, will send SIGKILL";
      }
    }

    if (IsProcessAlive()) {
      LOG(INFO) << "Killing " << ProcessNameAndPidStr() << " with SIGKILL";
      ignore_result(process_->Kill(SIGKILL));
    }
  }
  int ret;
  WARN_NOT_OK(process_->Wait(&ret), "Waiting on " + exe_);
  process_.reset();
}

void ExternalDaemon::FlushCoverage() {
#ifndef COVERAGE_BUILD_
  return;
#else
  LOG(INFO) << "Attempting to flush coverage for " << exe_ << " pid " << process_->pid();
  server::GenericServiceProxy proxy(messenger_, bound_rpc_addr());

  server::FlushCoverageRequestPB req;
  server::FlushCoverageResponsePB resp;
  rpc::RpcController rpc;

  // Set a reasonably short timeout, since some of our tests kill servers which
  // are kill -STOPed.
  rpc.set_timeout(MonoDelta::FromMilliseconds(100));
  Status s = proxy.FlushCoverage(req, &resp, &rpc);
  if (s.ok() && !resp.success()) {
    s = STATUS(RemoteError, "Server does not appear to be running a coverage build");
  }
  WARN_NOT_OK(s, Substitute("Unable to flush coverage on $0 pid $1", exe_, process_->pid()));
#endif
}

std::string ExternalDaemon::ProcessNameAndPidStr() {
  return Substitute("$0 with pid $1", exe_, process_->pid());
}

HostPort ExternalDaemon::bound_rpc_hostport() const {
  CHECK(status_);
  CHECK_GE(status_->bound_rpc_addresses_size(), 1);
  HostPort ret;
  CHECK_OK(HostPortFromPB(status_->bound_rpc_addresses(0), &ret));
  return ret;
}

Endpoint ExternalDaemon::bound_rpc_addr() const {
  HostPort hp = bound_rpc_hostport();
  std::vector<Endpoint> addrs;
  CHECK_OK(hp.ResolveAddresses(&addrs));
  CHECK(!addrs.empty());
  return addrs[0];
}

HostPort ExternalDaemon::bound_http_hostport() const {
  CHECK(status_);
  CHECK_GE(status_->bound_http_addresses_size(), 1);
  HostPort ret;
  CHECK_OK(HostPortFromPB(status_->bound_http_addresses(0), &ret));
  return ret;
}

const NodeInstancePB& ExternalDaemon::instance_id() const {
  CHECK(status_);
  return status_->node_instance();
}

const string& ExternalDaemon::uuid() const {
  CHECK(status_);
  return status_->node_instance().permanent_uuid();
}

Status ExternalDaemon::GetInt64Metric(const MetricEntityPrototype* entity_proto,
                                      const char* entity_id,
                                      const MetricPrototype* metric_proto,
                                      const char* value_field,
                                      int64_t* value) const {
  // Fetch metrics whose name matches the given prototype.
  string url = Substitute(
      "http://$0/jsonmetricz?metrics=$1",
      bound_http_hostport().ToString(),
      metric_proto->name());
  EasyCurl curl;
  faststring dst;
  RETURN_NOT_OK(curl.FetchURL(url, &dst));

  // Parse the results, beginning with the top-level entity array.
  JsonReader r(dst.ToString());
  RETURN_NOT_OK(r.Init());
  vector<const Value*> entities;
  RETURN_NOT_OK(r.ExtractObjectArray(r.root(), NULL, &entities));
  for (const Value* entity : entities) {
    // Find the desired entity.
    string type;
    RETURN_NOT_OK(r.ExtractString(entity, "type", &type));
    if (type != entity_proto->name()) {
      continue;
    }
    if (entity_id) {
      string id;
      RETURN_NOT_OK(r.ExtractString(entity, "id", &id));
      if (id != entity_id) {
        continue;
      }
    }

    // Find the desired metric within the entity.
    vector<const Value*> metrics;
    RETURN_NOT_OK(r.ExtractObjectArray(entity, "metrics", &metrics));
    for (const Value* metric : metrics) {
      string name;
      RETURN_NOT_OK(r.ExtractString(metric, "name", &name));
      if (name != metric_proto->name()) {
        continue;
      }
      RETURN_NOT_OK(r.ExtractInt64(metric, value_field, value));
      return Status::OK();
    }
  }
  string msg;
  if (entity_id) {
    msg = Substitute("Could not find metric $0.$1 for entity $2",
                     entity_proto->name(), metric_proto->name(),
                     entity_id);
  } else {
    msg = Substitute("Could not find metric $0.$1",
                     entity_proto->name(), metric_proto->name());
  }
  return STATUS(NotFound, msg);
}

string ExternalDaemon::LogPrefix() {
  return Format("{ daemon_id: $0 bound_rpc: $1 } ", daemon_id_, bound_rpc_);
}

//------------------------------------------------------------
// ScopedResumeExternalDaemon
//------------------------------------------------------------

ScopedResumeExternalDaemon::ScopedResumeExternalDaemon(ExternalDaemon* daemon)
    : daemon_(CHECK_NOTNULL(daemon)) {
}

ScopedResumeExternalDaemon::~ScopedResumeExternalDaemon() {
  CHECK_OK(daemon_->Resume());
}

//------------------------------------------------------------
// ExternalMaster
//------------------------------------------------------------
ExternalMaster::ExternalMaster(
    int master_index,
    const std::shared_ptr<rpc::Messenger>& messenger,
    const string& exe,
    const string& data_dir,
    const std::vector<string>& extra_flags,
    const string& rpc_bind_address,
    uint16_t http_port,
    const string& master_addrs)
    : ExternalDaemon(
          Substitute("m-$0", master_index + 1), messenger, exe, data_dir, "master", extra_flags),
      rpc_bind_address_(std::move(rpc_bind_address)),
      master_addrs_(std::move(master_addrs)),
      http_port_(http_port) {
}

ExternalMaster::~ExternalMaster() {
}

Status ExternalMaster::Start(bool shell_mode) {
  vector<string> flags;
  flags.push_back("--fs_data_dirs=" + data_dir_);
  flags.push_back("--rpc_bind_addresses=" + rpc_bind_address_);
  flags.push_back("--webserver_interface=localhost");
  flags.push_back(Substitute("--webserver_port=$0", http_port_));
  // On first start, we need to tell the masters their list of expected peers.
  // For 'shell' master, there is no master addresses.
  if (!shell_mode) {
    flags.push_back("--master_addresses=" + master_addrs_);
  }
  RETURN_NOT_OK(StartProcess(flags));
  return Status::OK();
}

Status ExternalMaster::Restart() {
  LOG_WITH_PREFIX(INFO) << "Restart()";
  if (!IsProcessAlive()) {
    // Make sure this function could be safely called if the process has already crashed.
    Shutdown();
  }
  // We store the addresses on shutdown so make sure we did that first.
  if (bound_rpc_.port() == 0) {
    return STATUS(IllegalState, "Master cannot be restarted. Must call Shutdown() first.");
  }
  return Start(true);
}

//------------------------------------------------------------
// ExternalTabletServer
//------------------------------------------------------------

ExternalTabletServer::ExternalTabletServer(
    int tablet_server_index, const std::shared_ptr<rpc::Messenger>& messenger,
    const std::string& exe, const std::string& data_dir, std::string bind_host, uint16_t rpc_port,
    uint16_t http_port, uint16_t redis_rpc_port, uint16_t redis_http_port,
    uint16_t cql_rpc_port, uint16_t cql_http_port,
    uint16_t pgsql_rpc_port, uint16_t pgsql_http_port,
    const std::vector<HostPort>& master_addrs, const std::vector<std::string>& extra_flags)
    : ExternalDaemon(
          Substitute("ts-$0", tablet_server_index + 1), messenger, exe, data_dir, "tserver",
          extra_flags),
      master_addrs_(HostPort::ToCommaSeparatedString(master_addrs)),
      bind_host_(std::move(bind_host)),
      rpc_port_(rpc_port),
      http_port_(http_port),
      redis_rpc_port_(redis_rpc_port),
      redis_http_port_(redis_http_port),
      pgsql_rpc_port_(pgsql_rpc_port),
      pgsql_http_port_(pgsql_http_port),
      cql_rpc_port_(cql_rpc_port),
      cql_http_port_(cql_http_port) {}

ExternalTabletServer::~ExternalTabletServer() {
}

Status ExternalTabletServer::Start(bool start_cql_proxy) {
  vector<string> flags;
  start_cql_proxy_ = start_cql_proxy;
  flags.push_back("--fs_data_dirs=" + data_dir_);
  flags.push_back(Substitute("--rpc_bind_addresses=$0:$1",
                             bind_host_, rpc_port_));
  flags.push_back(Substitute("--webserver_interface=$0",
                             bind_host_));
  flags.push_back(Substitute("--webserver_port=$0", http_port_));
  flags.push_back(Substitute("--redis_proxy_bind_address=$0:$1", bind_host_, redis_rpc_port_));
  flags.push_back(Substitute("--redis_proxy_webserver_port=$0", redis_http_port_));
  flags.push_back(Substitute("--pgsql_proxy_bind_address=$0:$1", bind_host_, pgsql_rpc_port_));
  flags.push_back(Substitute("--pgsql_proxy_webserver_port=$0", pgsql_http_port_));
  flags.push_back(Substitute("--cql_proxy_bind_address=$0:$1", bind_host_, cql_rpc_port_));
  flags.push_back(Substitute("--cql_proxy_webserver_port=$0", cql_http_port_));
  flags.push_back(Substitute("--start_cql_proxy=$0", start_cql_proxy_));
  flags.push_back("--tserver_master_addrs=" + master_addrs_);

  // Use conservative number of threads for the mini cluster for unit test env
  // where several unit tests tend to run in parallel.
  flags.push_back("--tablet_server_svc_num_threads=64");
  flags.push_back("--ts_consensus_svc_num_threads=20");

  RETURN_NOT_OK(StartProcess(flags));

  return Status::OK();
}

Status ExternalTabletServer::BuildServerStateFromInfoPath() {
  RETURN_NOT_OK(ExternalDaemon::BuildServerStateFromInfoPath());
  if (start_cql_proxy_) {
    RETURN_NOT_OK(ExternalDaemon::BuildServerStateFromInfoPath(GetCQLServerInfoPath(),
                                                               &cqlserver_status_));
  }
  return Status::OK();
}

string ExternalTabletServer::GetCQLServerInfoPath() {
  return ExternalDaemon::GetServerInfoPath() + "-cql";
}

bool ExternalTabletServer::ServerInfoPathsExist() {
  if (start_cql_proxy_) {
    return ExternalDaemon::ServerInfoPathsExist() &&
        Env::Default()->FileExists(GetCQLServerInfoPath());
  }
  return ExternalDaemon::ServerInfoPathsExist();
}

Status ExternalTabletServer::DeleteServerInfoPaths() {
  // We want to try a deletion for both files.
  Status s1 = ExternalDaemon::DeleteServerInfoPaths();
  Status s2 = Env::Default()->DeleteFile(GetCQLServerInfoPath());
  RETURN_NOT_OK(s1);
  RETURN_NOT_OK(s2);
  return Status::OK();
}

Status ExternalTabletServer::Restart(bool start_cql_proxy) {
  LOG_WITH_PREFIX(INFO) << "Restart: start_cql_proxy=" << start_cql_proxy;
  if (!IsProcessAlive()) {
    // Make sure this function could be safely called if the process has already crashed.
    Shutdown();
  }
  // We store the addresses on shutdown so make sure we did that first.
  if (bound_rpc_.port() == 0) {
    return STATUS(IllegalState, "Tablet server cannot be restarted. Must call Shutdown() first.");
  }
  return Start(start_cql_proxy);
}

}  // namespace yb
