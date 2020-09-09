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

#include <sys/stat.h>

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
#include "yb/tserver/tserver_service.proxy.h"
#include "yb/rpc/connection_context.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/yb_rpc.h"
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
#include "yb/yql/pgwrapper/pg_wrapper.h"

using namespace std::literals;  // NOLINT
using namespace yb::size_literals;  // NOLINT

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
using yb::master::IsInitDbDoneRequestPB;
using yb::master::IsInitDbDoneResponsePB;
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
using yb::master::GetMasterClusterConfigRequestPB;
using yb::master::GetMasterClusterConfigResponsePB;
using yb::master::ChangeMasterClusterConfigRequestPB;
using yb::master::ChangeMasterClusterConfigResponsePB;
using yb::master::SysClusterConfigEntryPB;
using yb::master::BlacklistPB;
using yb::master::ListMastersRequestPB;
using yb::master::ListMastersResponsePB;
using yb::master::ListMasterRaftPeersRequestPB;
using yb::master::ListMasterRaftPeersResponsePB;
using yb::tserver::TabletServerErrorPB;
using yb::rpc::RpcController;
using yb::pgwrapper::PgWrapper;

typedef ListTabletsResponsePB::StatusAndSchemaPB StatusAndSchemaPB;

DECLARE_string(vmodule);
DECLARE_int32(replication_factor);
DECLARE_bool(mem_tracker_logging);
DECLARE_bool(mem_tracker_log_stack_trace);
DECLARE_string(minicluster_daemon_id);
DECLARE_bool(use_libbacktrace);

DEFINE_string(external_daemon_heap_profile_prefix, "",
              "If this is not empty, tcmalloc's HEAPPROFILE is set this, followed by a unique "
              "suffix for external mini-cluster daemons.");

DEFINE_bool(external_daemon_safe_shutdown, false,
            "Shutdown external daemons using SIGTERM first. Disabled by default to avoid "
            "interfering with kill-testing.");

DECLARE_int64(outbound_rpc_block_size);
DECLARE_int64(outbound_rpc_memory_limit);

DEFINE_int64(external_mini_cluster_max_log_bytes, 50_MB * 100,
             "Max total size of log bytes produced by all external mini-cluster daemons. "
             "The test is shut down if this limit is exceeded.");

namespace yb {

static const char* const kMasterBinaryName = "yb-master";
static const char* const kTabletServerBinaryName = "yb-tserver";
static double kProcessStartTimeoutSeconds = 60.0;
static MonoDelta kTabletServerRegistrationTimeout = 60s;

static const int kHeapProfileSignal = SIGUSR1;

#if defined(__APPLE__)
static bool kBindToUniqueLoopbackAddress = false;
#else
static bool kBindToUniqueLoopbackAddress = true;
#endif

constexpr size_t kDefaultMemoryLimitHardBytes = NonTsanVsTsan(1_GB, 512_MB);

namespace {

void AddExtraFlagsFromEnvVar(const char* env_var_name, std::vector<std::string>* args_dest) {
  const char* extra_daemon_flags_env_var_value = getenv(env_var_name);
  if (extra_daemon_flags_env_var_value) {
    LOG(INFO) << "Setting extra daemon flags as specified by env var " << env_var_name << ": "
              << extra_daemon_flags_env_var_value;
    // TODO: this has an issue with handling quoted arguments with embedded spaces.
    std::istringstream iss(extra_daemon_flags_env_var_value);
    copy(std::istream_iterator<string>(iss),
         std::istream_iterator<string>(),
         std::back_inserter(*args_dest));
  } else {
    LOG(INFO) << "Env var " << env_var_name << " not specified, not setting extra flags from it";
  }
}

}  // anonymous namespace

// ------------------------------------------------------------------------------------------------
// ExternalMiniClusterOptions
// ------------------------------------------------------------------------------------------------

ExternalMiniClusterOptions::ExternalMiniClusterOptions()
    : bind_to_unique_loopback_addresses(kBindToUniqueLoopbackAddress),
      timeout(MonoDelta::FromMilliseconds(1000 * 10)) {
}

ExternalMiniClusterOptions::~ExternalMiniClusterOptions() {
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

void ExternalMiniClusterOptions::AdjustMasterRpcPorts() {
  if (master_rpc_ports.size() == 1 && master_rpc_ports[0] == 0) {
    // Add missing master ports to avoid errors when we try to start the cluster.
    while (master_rpc_ports.size() < num_masters) {
      master_rpc_ports.push_back(0);
    }
  }
}

// ------------------------------------------------------------------------------------------------
// ExternalMiniCluster
// ------------------------------------------------------------------------------------------------

ExternalMiniCluster::ExternalMiniCluster(const ExternalMiniClusterOptions& opts)
    : opts_(opts), add_new_master_at_(-1) {
  opts_.AdjustMasterRpcPorts();
  // These "extra mini cluster options" are added in the end of the command line.
  const auto common_extra_flags = {
      "--enable_tracing"s,
      Substitute("--memory_limit_hard_bytes=$0", kDefaultMemoryLimitHardBytes),
      (opts.log_to_file ? "--alsologtostderr"s : "--logtostderr"s)
  };
  for (auto* extra_flags : {&opts_.extra_master_flags, &opts_.extra_tserver_flags}) {
    // Common default extra flags are inserted in the beginning so that they can be overridden by
    // caller-specified flags.
    extra_flags->insert(extra_flags->begin(),
                        common_extra_flags.begin(),
                        common_extra_flags.end());
  }
  AddExtraFlagsFromEnvVar("YB_EXTRA_MASTER_FLAGS", &opts_.extra_master_flags);
  AddExtraFlagsFromEnvVar("YB_EXTRA_TSERVER_FLAGS", &opts_.extra_tserver_flags);
}

ExternalMiniCluster::~ExternalMiniCluster() {
  Shutdown();
  if (messenger_holder_) {
    messenger_holder_->Shutdown();
  }
}

Status ExternalMiniCluster::DeduceBinRoot(std::string* ret) {
  string exe;
  RETURN_NOT_OK(Env::Default()->GetExecutablePath(&exe));
  *ret = DirName(exe) + "/../bin";
  return Status::OK();
}

std::string ExternalMiniCluster::GetClusterDataDirName() const {
  if (opts_.cluster_id == "") {
    return "minicluster-data";
  }
  return Format("minicluster-data-$0", opts_.cluster_id);
}

Status ExternalMiniCluster::HandleOptions() {
  daemon_bin_path_ = opts_.daemon_bin_path;
  if (daemon_bin_path_.empty()) {
    RETURN_NOT_OK(DeduceBinRoot(&daemon_bin_path_));
  }

  data_root_ = opts_.data_root;
  if (data_root_.empty()) {
    // If they don't specify a data root, use the current gtest directory.
    data_root_ = JoinPathSegments(GetTestDataDirectory(), GetClusterDataDirName());

    // If "data_root_counter" is non-negative, and the auto-generated "data_root_" directory already
    // exists, create a subdirectory using the counter value as its name. The caller should maintain
    // this counter and increment it for each test run.
    if (opts_.data_root_counter >= 0) {
      struct stat sb;
      if (stat(data_root_.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) {
        data_root_ = Substitute("$0/$1", data_root_, opts_.data_root_counter);
        CHECK_EQ(mkdir(data_root_.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH), 0);
      }
    }
  }

  return Status::OK();
}

Status ExternalMiniCluster::Start(rpc::Messenger* messenger) {
  CHECK(masters_.empty()) << "Masters are not empty (size: " << masters_.size()
      << "). Maybe you meant Restart()?";
  CHECK(tablet_servers_.empty()) << "Tablet servers are not empty (size: "
      << tablet_servers_.size() << "). Maybe you meant Restart()?";
  RETURN_NOT_OK(HandleOptions());
  FLAGS_replication_factor = opts_.num_masters;

  if (messenger == nullptr) {
    rpc::MessengerBuilder builder("minicluster-messenger");
    builder.set_num_reactors(1);
    messenger_holder_ = VERIFY_RESULT_PREPEND(
        builder.Build(), "Failed to start Messenger for minicluster");
    messenger_ = messenger_holder_.get();
  } else {
    messenger_holder_ = nullptr;
    messenger_ = messenger;
  }
  proxy_cache_ = std::make_unique<rpc::ProxyCache>(messenger_);

  Status s = Env::Default()->CreateDir(data_root_);
  if (!s.ok() && !s.IsAlreadyPresent()) {
    RETURN_NOT_OK_PREPEND(s, "Could not create root dir " + data_root_);
  }

  LOG(INFO) << "Starting cluster with option bind_to_unique_loopback_addresses="
      << (opts_.bind_to_unique_loopback_addresses ? "true" : "false");

  LOG(INFO) << "Starting " << opts_.num_masters << " masters";
  RETURN_NOT_OK_PREPEND(StartMasters(), "Failed to start masters.");
  add_new_master_at_ = opts_.num_masters;

  if (opts_.num_tablet_servers > 0) {
    LOG(INFO) << "Starting " << opts_.num_tablet_servers << " tablet servers";

    for (int i = 1; i <= opts_.num_tablet_servers; i++) {
      RETURN_NOT_OK_PREPEND(
          AddTabletServer(ExternalMiniClusterOptions::kDefaultStartCqlProxy),
          Substitute("Failed starting tablet server $0", i));
    }
    RETURN_NOT_OK(WaitForTabletServerCount(
        opts_.num_tablet_servers, kTabletServerRegistrationTimeout));
  } else {
    LOG(INFO) << "No need to start tablet servers";
  }

  running_ = true;
  return Status::OK();
}

void ExternalMiniCluster::Shutdown(NodeSelectionMode mode) {
  // TODO: in the regular MiniCluster Shutdown is a no-op if running_ is false.
  // Therefore, in case of an error during cluster startup behavior might be different.
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
  running_ = false;
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

  RETURN_NOT_OK(WaitForTabletServerCount(tablet_servers_.size(), kTabletServerRegistrationTimeout));

  running_ = true;
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

Result<ExternalMaster *> ExternalMiniCluster::StartMasterWithPeers(const string& peer_addrs) {
  uint16_t rpc_port = AllocateFreePort();
  uint16_t http_port = AllocateFreePort();
  LOG(INFO) << "Using auto-assigned rpc_port " << rpc_port << "; http_port " << http_port
            << " to start a new external mini-cluster master with peers '" << peer_addrs << "'.";

  string addr = MasterAddressForPort(rpc_port);
  string exe = GetBinaryPath(kMasterBinaryName);

  ExternalMaster* master =
      new ExternalMaster(add_new_master_at_, messenger_, proxy_cache_.get(), exe,
                         GetDataPath(Substitute("master-$0", add_new_master_at_)),
                         opts_.extra_master_flags, addr, http_port, peer_addrs);

  RETURN_NOT_OK(master->Start());

  add_new_master_at_++;
  return master;
}

std::string ExternalMiniCluster::MasterAddressForPort(uint16_t port) const {
  return Format(opts_.use_even_ips ? "127.0.0.2:$0" : "127.0.0.1:$0", port);
}

void ExternalMiniCluster::StartShellMaster(ExternalMaster** new_master) {
  uint16_t rpc_port = AllocateFreePort();
  uint16_t http_port = AllocateFreePort();
  LOG(INFO) << "Using auto-assigned rpc_port " << rpc_port << "; http_port " << http_port
            << " to start a new external mini-cluster shell master.";

  string addr = MasterAddressForPort(rpc_port);

  string exe = GetBinaryPath(kMasterBinaryName);

  ExternalMaster* master = new ExternalMaster(
      add_new_master_at_,
      messenger_,
      proxy_cache_.get(),
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

  return std::make_shared<ConsensusServiceProxy>(proxy_cache_.get(), leader_master_sock);
}

std::shared_ptr<ConsensusServiceProxy> ExternalMiniCluster::GetConsensusProxy(
    scoped_refptr<ExternalMaster> master) {
  auto master_sock = master->bound_rpc_addr();

  return std::make_shared<ConsensusServiceProxy>(proxy_cache_.get(), master_sock);
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
  lsd_rpc.set_timeout(opts_.timeout);
  ConsensusServiceProxy proxy(proxy_cache_.get(), host_port);
  RETURN_NOT_OK(proxy.LeaderStepDown(lsd_req, &lsd_resp, &lsd_rpc));
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
                                         RaftPeerPB::MemberType member_type,
                                         bool use_hostport) {
  if (type != consensus::ADD_SERVER && type != consensus::REMOVE_SERVER) {
    return STATUS(InvalidArgument, Substitute("Invalid Change Config type $0", type));
  }

  ChangeConfigRequestPB req;
  ChangeConfigResponsePB resp;
  rpc::RpcController rpc;
  rpc.set_timeout(opts_.timeout);

  RaftPeerPB peer_pb;
  peer_pb.set_permanent_uuid(use_hostport ? "" : master->uuid());
  if (type == consensus::ADD_SERVER) {
    peer_pb.set_member_type(member_type);
  }
  HostPortToPB(master->bound_rpc_hostport(), peer_pb.mutable_last_known_private_addr()->Add());
  req.set_tablet_id(yb::master::kSysCatalogTabletId);
  req.set_type(type);
  req.set_use_host(use_hostport);
  *req.mutable_server() = peer_pb;

  // There could be timing window where we found the leader host/port, but an election in the
  // meantime could have made it step down. So we retry till we hit the leader correctly.
  int num_attempts = 1;
  while (true) {
    ExternalMaster* leader = GetLeaderMaster();
    auto leader_proxy = std::make_unique<ConsensusServiceProxy>(
        proxy_cache_.get(), leader->bound_rpc_addr());
    string leader_uuid = leader->uuid();

    if (type == consensus::REMOVE_SERVER && leader_uuid == req.server().permanent_uuid()) {
      RETURN_NOT_OK(StepDownMasterLeaderAndWaitForNewLeader());
      leader = GetLeaderMaster();
      leader_uuid = leader->uuid();
      leader_proxy.reset(new ConsensusServiceProxy(proxy_cache_.get(), leader->bound_rpc_addr()));
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

Status ExternalMiniCluster::PingMaster(ExternalMaster* master) const {
  int index = GetIndexOfMaster(master);
  server::PingRequestPB req;
  server::PingResponsePB resp;
  std::shared_ptr<server::GenericServiceProxy> proxy =
      index == -1 ? master_generic_proxy(master->bound_rpc_addr()) : master_generic_proxy(index);
  rpc::RpcController rpc;
  rpc.set_timeout(opts_.timeout);
  return proxy->Ping(req, &resp, &rpc);
}

Status ExternalMiniCluster::AddTServerToBlacklist(
    ExternalMaster* master,
    ExternalTabletServer* ts) {
  GetMasterClusterConfigRequestPB config_req;
  GetMasterClusterConfigResponsePB config_resp;
  int index = GetIndexOfMaster(master);

  if (index == -1) {
    return STATUS(InvalidArgument, Substitute(
        "Given master '$0' not in the current list of $1 masters.",
        master->bound_rpc_hostport().ToString(), masters_.size()));
  }

  std::shared_ptr<MasterServiceProxy> proxy = master_proxy(index);
  rpc::RpcController rpc;
  rpc.set_timeout(opts_.timeout);
  RETURN_NOT_OK(proxy->GetMasterClusterConfig(config_req, &config_resp, &rpc));
  if (config_resp.has_error()) {
    return STATUS(RuntimeError, Substitute(
        "GetMasterClusterConfig RPC response hit error: $0",
        config_resp.error().ShortDebugString()));
  }
  // Get current config
  ChangeMasterClusterConfigRequestPB change_req;
  SysClusterConfigEntryPB config = *config_resp.mutable_cluster_config();
  // add tserver to blacklist
  HostPortToPB(ts->bound_rpc_hostport(), config.mutable_server_blacklist()->mutable_hosts()->Add());
  *change_req.mutable_cluster_config() = config;
  ChangeMasterClusterConfigResponsePB change_resp;
  rpc.Reset();
  RETURN_NOT_OK(proxy->ChangeMasterClusterConfig(change_req, &change_resp, &rpc));
  if (change_resp.has_error()) {
    return STATUS(RuntimeError, Substitute(
        "ChangeMasterClusterConfig RPC response hit error: $0",
        change_resp.error().ShortDebugString()));
  }

  LOG(INFO) << "TServer at " << ts->bound_rpc_hostport().ToString()
  << " was added to the blacklist";

  return Status::OK();
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
  rpc.set_timeout(opts_.timeout);
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
  OpIdPB start_opid;
  RETURN_NOT_OK(GetLastOpIdForLeader(&start_opid));
  LOG(INFO) << "Start OPID : " << start_opid.ShortDebugString();

  // Need not do any wait if it is a restart case - so the commit term will be > 0.
  if (start_opid.term() != 0)
    return Status::OK();

  MonoTime now = MonoTime::Now();
  MonoTime deadline = now;
  deadline.AddDelta(opts_.timeout);
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
    vector<OpIdPB>* op_ids) {
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
  auto deadline = CoarseMonoClock::Now() + opts_.timeout.ToSteadyDuration();

  for (int i = 1;; i++) {
    vector<OpIdPB> ids;
    Status s = GetLastOpIdForEachMasterPeer(opts_.timeout, consensus::COMMITTED_OPID, &ids);

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

    if (CoarseMonoClock::Now() >= deadline) {
      if (!s.ok()) {
        return s;
      }

      return STATUS_FORMAT(TimedOut,
                           "Index $0 not available on all replicas after $1. ",
                           target_index,
                           opts_.timeout);
    }

    SleepFor(MonoDelta::FromMilliseconds(min(i * 100, 1000)));
  }
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
  rpc.set_timeout(opts_.timeout);
  RETURN_NOT_OK(proxy->IsMasterLeaderServiceReady(req, &resp, &rpc));
  if (resp.has_error()) {
    return STATUS(RuntimeError, Substitute(
        "Is master ready RPC response hit error: $0", resp.error().ShortDebugString()));
  }

  return Status::OK();
}

Status ExternalMiniCluster::GetLastOpIdForLeader(OpIdPB* opid) {
  ExternalMaster* leader = GetLeaderMaster();
  auto leader_master_sock = leader->bound_rpc_addr();
  std::shared_ptr<ConsensusServiceProxy> leader_proxy =
    std::make_shared<ConsensusServiceProxy>(proxy_cache_.get(), leader_master_sock);

  RETURN_NOT_OK(itest::GetLastOpIdForMasterReplica(
      leader_proxy,
      yb::master::kSysCatalogTabletId,
      leader->uuid(),
      consensus::COMMITTED_OPID,
      opts_.timeout,
      opid));

  return Status::OK();
}

string ExternalMiniCluster::GetMasterAddresses() const {
  string peer_addrs = "";
  for (int i = 0; i < opts_.num_masters; i++) {
    if (!peer_addrs.empty()) {
      peer_addrs += ",";
    }
    peer_addrs += MasterAddressForPort(opts_.master_rpc_ports[i]);
  }
  return peer_addrs;
}

string ExternalMiniCluster::GetTabletServerAddresses() const {
  string peer_addrs = "";
  for (const auto& ts : tablet_servers_) {
    if (!peer_addrs.empty()) {
      peer_addrs += ",";
    }
    peer_addrs += HostPortToString(ts->bind_host(), ts->rpc_port());
  }
  return peer_addrs;
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
    string addr = MasterAddressForPort(opts_.master_rpc_ports[i]);
    peer_addrs.push_back(addr);
  }
  string peer_addrs_str = JoinStrings(peer_addrs, ",");
  vector<string> flags = opts_.extra_master_flags;
  flags.push_back("--enable_leader_failure_detection=true");
  if (opts_.enable_ysql) {
    flags.push_back("--enable_ysql=true");
    flags.push_back("--master_auto_run_initdb");
  } else {
    flags.push_back("--enable_ysql=false");
  }
  string exe = GetBinaryPath(kMasterBinaryName);

  // Start the masters.
  for (int i = 0; i < num_masters; i++) {
    uint16_t http_port = AllocateFreePort();
    scoped_refptr<ExternalMaster> peer =
      new ExternalMaster(
        i,
        messenger_,
        proxy_cache_.get(),
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

  if (opts_.enable_ysql) {
    RETURN_NOT_OK(WaitForInitDb());
  }
  return Status::OK();
}

Status ExternalMiniCluster::WaitForInitDb() {
  const auto start_time = std::chrono::steady_clock::now();
  const auto kTimeout = NonTsanVsTsan(900s, 1800s);
  while (true) {
    for (int i = 0; i < opts_.num_masters; i++) {
      auto elapsed_time = std::chrono::steady_clock::now() - start_time;
      if (elapsed_time > kTimeout) {
        return STATUS_FORMAT(
            TimedOut,
            "Timed out while waiting for initdb to complete: elapsed time is $0, timeout is $1",
            elapsed_time, kTimeout);
      }
      std::shared_ptr<MasterServiceProxy> proxy = master_proxy(i);
      rpc::RpcController rpc;
      rpc.set_timeout(opts_.timeout);
      IsInitDbDoneRequestPB req;
      IsInitDbDoneResponsePB resp;
      RETURN_NOT_OK(proxy->IsInitDbDone(req, &resp, &rpc));
      if (resp.has_error() &&
          resp.error().code() != master::MasterErrorPB::NOT_THE_LEADER) {

        return STATUS(RuntimeError, Substitute(
            "IsInitDbDone RPC response hit error: $0",
            resp.error().ShortDebugString()));
      }
      if (resp.done()) {
        if (resp.has_initdb_error() && !resp.initdb_error().empty()) {
          LOG(ERROR) << "master reported an initdb error: " << resp.initdb_error();
          return STATUS(RuntimeError, "initdb failed: " + resp.initdb_error());
        }
        LOG(INFO) << "master indicated that initdb is done";
        return Status::OK();
      }
    }
    std::this_thread::sleep_for(500ms);
  }
}

string ExternalMiniCluster::GetBindIpForTabletServer(int index) const {
  if (opts_.use_even_ips) {
    return Substitute("127.0.0.$0", (index + 1) * 2);
  } else if (opts_.bind_to_unique_loopback_addresses) {
#if defined(__APPLE__)
    return Substitute("127.0.0.$0", index + 1); // Use default 127.0.0.x IPs.
#else
    const pid_t p = getpid();
    return Substitute("127.$0.$1.$2", (p >> 8) & 0xff, p & 0xff, index);
#endif
  } else {
    return "127.0.0.1";
  }
}

Status ExternalMiniCluster::AddTabletServer(
    bool start_cql_proxy, const std::vector<std::string>& extra_flags) {
  CHECK(GetLeaderMaster() != nullptr)
      << "Must have started at least 1 master before adding tablet servers";

  int idx = tablet_servers_.size();

  string exe = GetBinaryPath(kTabletServerBinaryName);
  vector<HostPort> master_hostports;
  for (int i = 0; i < num_masters(); i++) {
    master_hostports.push_back(DCHECK_NOTNULL(master(i))->bound_rpc_hostport());
  }

  uint16_t ts_rpc_port = 0;
  uint16_t ts_http_port = 0;
  uint16_t redis_rpc_port = 0;
  uint16_t redis_http_port = 0;
  uint16_t cql_rpc_port = 0;
  uint16_t cql_http_port = 0;
  uint16_t pgsql_rpc_port = 0;
  uint16_t pgsql_http_port = 0;

  if (idx > 0 && opts_.use_same_ts_ports && opts_.bind_to_unique_loopback_addresses) {
    const scoped_refptr<ExternalTabletServer>& first_ts = tablet_servers_[0];
    ts_rpc_port = first_ts->rpc_port();
    ts_http_port = first_ts->http_port();
    redis_rpc_port = first_ts->redis_rpc_port();
    redis_http_port = first_ts->redis_http_port();
    cql_rpc_port = first_ts->cql_rpc_port();
    cql_http_port = first_ts->cql_http_port();
    pgsql_rpc_port = first_ts->pgsql_rpc_port();
    pgsql_http_port = first_ts->pgsql_http_port();
  } else {
    ts_rpc_port = AllocateFreePort();
    ts_http_port = AllocateFreePort();
    redis_rpc_port = AllocateFreePort();
    redis_http_port = AllocateFreePort();
    cql_rpc_port = AllocateFreePort();
    cql_http_port = AllocateFreePort();
    pgsql_rpc_port = AllocateFreePort();
    pgsql_http_port = AllocateFreePort();
  }

  vector<string> flags = opts_.extra_tserver_flags;
  if (opts_.enable_ysql) {
    flags.push_back("--enable_ysql=true");
  } else {
    flags.push_back("--enable_ysql=false");
  }
  flags.insert(flags.end(), extra_flags.begin(), extra_flags.end());

  scoped_refptr<ExternalTabletServer> ts = new ExternalTabletServer(
      idx, messenger_, proxy_cache_.get(), exe, GetDataPath(Substitute("ts-$0", idx + 1)),
      GetBindIpForTabletServer(idx), ts_rpc_port, ts_http_port, redis_rpc_port, redis_http_port,
      cql_rpc_port, cql_http_port, pgsql_rpc_port, pgsql_http_port, master_hostports,
      SubstituteInFlags(flags, idx));
  RETURN_NOT_OK(ts->Start(start_cql_proxy));
  tablet_servers_.push_back(ts);
  return Status::OK();
}

Status ExternalMiniCluster::WaitForTabletServerCount(int count, const MonoDelta& timeout) {
  MonoTime deadline = MonoTime::Now();
  deadline.AddDelta(timeout);

  std::vector<scoped_refptr<ExternalTabletServer>> last_unmatched = tablet_servers_;
  bool had_leader = false;

  while (true) {
    MonoDelta remaining = deadline - MonoTime::Now();
    if (remaining.ToSeconds() < 0) {
      std::vector<std::string> unmatched_uuids;
      unmatched_uuids.reserve(last_unmatched.size());
      for (const auto& server : last_unmatched) {
        unmatched_uuids.push_back(server->instance_id().permanent_uuid());
      }
      if (!had_leader) {
        return STATUS(TimedOut, "Does not have active master leader to check tserver registration");
      }
      return STATUS_FORMAT(TimedOut, "$0 TS(s) never registered with master (not registered $1)",
                           count, unmatched_uuids);
    }

    // We should give some time for RPC to proceed, otherwise all requests would fail.
    remaining = std::max<MonoDelta>(remaining, 250ms);

    last_unmatched = tablet_servers_;
    had_leader = false;
    for (int i = 0; i < masters_.size(); i++) {
      master::ListTabletServersRequestPB req;
      master::ListTabletServersResponsePB resp;
      rpc::RpcController rpc;
      rpc.set_timeout(remaining);
      auto status = master_proxy(i)->ListTabletServers(req, &resp, &rpc);
      LOG_IF(WARNING, !status.ok()) << "ListTabletServers failed: " << status;
      if (!status.ok() || resp.has_error()) {
        continue;
      }
      had_leader = true;
      // ListTabletServers() may return servers that are no longer online.
      // Do a second step of verification to verify that the descs that we got
      // are aligned (same uuid/seqno) with the TSs that we have in the cluster.
      int match_count = 0;
      for (const master::ListTabletServersResponsePB_Entry& e : resp.servers()) {
        for (auto it = last_unmatched.begin(); it != last_unmatched.end(); ++it) {
          if ((**it).instance_id().permanent_uuid() == e.instance_id().permanent_uuid() &&
              (**it).instance_id().instance_seqno() == e.instance_id().instance_seqno()) {
            match_count++;
            last_unmatched.erase(it);
            break;
          }
        }
      }
      if (match_count >= count) {
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
    EXPECT_TRUE(d->IsProcessAlive()) << "At least one process crashed. viz: "
                                     << d->id();
  }
}

Result<std::vector<std::string>> ExternalMiniCluster::GetTabletIds(ExternalTabletServer* ts) {
  TabletServerServiceProxy proxy(proxy_cache_.get(), ts->bound_rpc_addr());
  ListTabletsRequestPB req;
  ListTabletsResponsePB resp;

  rpc::RpcController rpc;
  rpc.set_timeout(MonoDelta::FromSeconds(10));
  RETURN_NOT_OK(proxy.ListTablets(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  std::vector<std::string> result;
  result.reserve(resp.status_and_schema().size());
  for (const StatusAndSchemaPB& status : resp.status_and_schema()) {
    result.push_back(status.tablet_status().tablet_id());
  }

  return result;
}

Status ExternalMiniCluster::WaitForTabletsRunning(ExternalTabletServer* ts,
                                                  const MonoDelta& timeout) {
  TabletServerServiceProxy proxy(proxy_cache_.get(), ts->bound_rpc_addr());
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
  server::MasterAddresses addrs;
  HostPort leader_master_hp;
  auto deadline = CoarseMonoClock::Now() + 5s;

  *idx = 0;  // default to 0'th index, even in case of errors.

  for (const scoped_refptr<ExternalMaster>& master : masters_) {
    if (master->IsProcessAlive()) {
      addrs.push_back({ master->bound_rpc_addr() });
    }
  }
  if (addrs.empty()) {
    return STATUS(IllegalState, "No running masters");
  }
  rpc::Rpcs rpcs;
  auto rpc = rpc::StartRpc<GetLeaderMasterRpc>(
      Bind(&LeaderMasterCallback, &leader_master_hp, &sync),
      addrs,
      deadline,
      messenger_,
      proxy_cache_.get(),
      &rpcs);
  RETURN_NOT_OK(sync.Wait());
  rpcs.Shutdown();
  bool found = false;
  for (int i = 0; i < masters_.size(); i++) {
    bool matches_leader = masters_[i]->bound_rpc_hostport().port() == leader_master_hp.port();
    if (is_leader == matches_leader) {
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

vector<ExternalMaster*> ExternalMiniCluster::master_daemons() const {
  vector<ExternalMaster*> results;
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

std::vector<ExternalTabletServer*> ExternalMiniCluster::tserver_daemons() const {
  std::vector<ExternalTabletServer*> result;
  result.reserve(tablet_servers_.size());
  for (const auto& ts : tablet_servers_) {
    result.push_back(ts.get());
  }
  return result;
}

HostPort ExternalMiniCluster::pgsql_hostport(int node_index) const {
  return HostPort(tablet_servers_[node_index]->bind_host(),
                  tablet_servers_[node_index]->pgsql_rpc_port());
}

rpc::Messenger* ExternalMiniCluster::messenger() {
  return messenger_;
}

std::shared_ptr<MasterServiceProxy> ExternalMiniCluster::master_proxy() {
  CHECK_EQ(masters_.size(), 1);
  return master_proxy(0);
}

std::shared_ptr<MasterServiceProxy> ExternalMiniCluster::master_proxy(int idx) {
  CHECK_GE(idx, 0);
  CHECK_LT(idx, masters_.size());
  return std::make_shared<MasterServiceProxy>(
      proxy_cache_.get(), CHECK_NOTNULL(master(idx))->bound_rpc_addr());
}

std::shared_ptr<server::GenericServiceProxy> ExternalMiniCluster::master_generic_proxy(
    int idx) const {
  CHECK_GE(idx, 0);
  CHECK_LT(idx, masters_.size());
  return std::make_shared<server::GenericServiceProxy>(
    proxy_cache_.get(), CHECK_NOTNULL(master(idx))->bound_rpc_addr());
}

std::shared_ptr<server::GenericServiceProxy> ExternalMiniCluster::master_generic_proxy(
    const HostPort& bound_rpc_addr) const {
  return std::make_shared<server::GenericServiceProxy>(proxy_cache_.get(), bound_rpc_addr);
}

void ExternalMiniCluster::ConfigureClientBuilder(client::YBClientBuilder* builder) {
  CHECK_NOTNULL(builder);
  CHECK(!masters_.empty());
  builder->clear_master_server_addrs();
  for (const scoped_refptr<ExternalMaster>& master : masters_) {
    builder->add_master_server_addr(master->bound_rpc_hostport().ToString());
  }
}

HostPort ExternalMiniCluster::DoGetLeaderMasterBoundRpcAddr() {
  return GetLeaderMaster()->bound_rpc_addr();
}

Status ExternalMiniCluster::SetFlag(ExternalDaemon* daemon,
                                    const string& flag,
                                    const string& value) {
  server::GenericServiceProxy proxy(proxy_cache_.get(), daemon->bound_rpc_addr());

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
  auto master_proxy = std::make_shared<ConsensusServiceProxy>(proxy_cache_.get(), master_sock);

  RunLeaderElectionRequestPB req;
  req.set_dest_uuid(master->uuid());
  req.set_tablet_id(yb::master::kSysCatalogTabletId);
  RunLeaderElectionResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(opts_.timeout);
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
  atomic<int> next_log_tailer_id{0};

  // We need some references to these heap-allocated atomic booleans so that ASAN would not consider
  // them memory leaks.
  mutex id_to_stopped_flag_mutex;
  map<int, atomic<bool>*> id_to_stopped_flag;

  // This is used to limit the total amount of logs produced by external daemons over the lifetime
  // of a test program. Guarded by logging_mutex.
  size_t total_bytes_logged = 0;
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
          auto& logging_mutex = global_state()->logging_mutex;
          auto& total_bytes_logged = global_state()->total_bytes_logged;
          while (!(is_eof = feof(fp)) &&
                 !(is_fgets_null = (fgets(buf, sizeof(buf), fp) == nullptr)) &&
                 !stopped->load()) {
            size_t l = strlen(buf);
            const char* maybe_end_of_line = l > 0 && buf[l - 1] == '\n' ? "" : "\n";
            // Synchronize tailing output from all external daemons for simplicity.
            lock_guard<mutex> lock(logging_mutex);
            if (stopped->load()) break;
            // Make sure we always output an end-of-line character.
            *out << line_prefix << " " << buf << maybe_end_of_line;
            if (!stopped->load()) {
              auto listener = listener_.load(std::memory_order_acquire);
              if (!stopped->load() && listener) {
                listener->Handle(GStringPiece(buf, maybe_end_of_line ? l : l - 1));
              }
            }
            total_bytes_logged += strlen(buf) + strlen(maybe_end_of_line);
            // Abort the test if it produces too much log spew.
            CHECK_LE(total_bytes_logged, FLAGS_external_mini_cluster_max_log_bytes);
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

  void SetListener(StringListener* listener) {
    listener_ = listener;
  }

  void RemoveListener(StringListener* listener) {
    listener_.compare_exchange_strong(listener, nullptr);
  }

  ~LogTailerThread() {
    VLOG(1) << "Stopping " << thread_desc_;
    lock_guard<mutex> l(state_lock_);
    stopped_->store(true);
    listener_ = nullptr;
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
  std::atomic<StringListener*> listener_{nullptr};
};

ExternalDaemon::ExternalDaemon(
    std::string daemon_id,
    rpc::Messenger* messenger,
    rpc::ProxyCache* proxy_cache,
    string exe,
    string data_dir,
    string server_type,
    vector<string> extra_flags)
  : daemon_id_(daemon_id),
    messenger_(messenger),
    proxy_cache_(proxy_cache),
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

  // Disabled due to #4507.
  // TODO: Enable metrics logging after #4507 is fixed.
  //
  // Even though we set -logtostderr down below, metrics logs end up being written
  // based on -log_dir. So, we have to set that too.
  argv.push_back("--metrics_log_interval_ms=0");

  // Force set log_dir to empty value, process will chose default destination inside fs_data_dir
  // In other case log_dir value will be extracted from TEST_TMPDIR env variable but it is
  // inherited from test script
  argv.push_back("--log_dir=");

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
  if (!s.ok() && !s.IsNotFound()) {
    LOG (WARNING) << "Failed to delete info paths: " << s.ToString();
  }

  // Ensure that logging goes to the test output doesn't get buffered.
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
  if (FLAGS_use_libbacktrace) {
    argv.push_back("--use_libbacktrace");
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

  AddExtraFlagsFromEnvVar("YB_EXTRA_DAEMON_FLAGS", &argv);

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
  int ret = 0;
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
  return HostPortFromPB(status_->bound_rpc_addresses(0));
}

HostPort ExternalDaemon::bound_rpc_addr() const {
  return bound_rpc_hostport();
}

HostPort ExternalDaemon::bound_http_hostport() const {
  CHECK(status_);
  CHECK_GE(status_->bound_http_addresses_size(), 1);
  return HostPortFromPB(status_->bound_http_addresses(0));
}

const NodeInstancePB& ExternalDaemon::instance_id() const {
  CHECK(status_);
  return status_->node_instance();
}

const string& ExternalDaemon::uuid() const {
  CHECK(status_);
  return status_->node_instance().permanent_uuid();
}

Result<int64_t> ExternalDaemon::GetInt64MetricFromHost(const HostPort& hostport,
                                                       const MetricEntityPrototype* entity_proto,
                                                       const char* entity_id,
                                                       const MetricPrototype* metric_proto,
                                                       const char* value_field) {
  return GetInt64MetricFromHost(hostport, entity_proto->name(), entity_id, metric_proto->name(),
                                value_field);
}

Result<int64_t> ExternalDaemon::GetInt64MetricFromHost(const HostPort& hostport,
                                                       const char* entity_proto_name,
                                                       const char* entity_id,
                                                       const char* metric_proto_name,
                                                       const char* value_field) {
  // Fetch metrics whose name matches the given prototype.
  string url = Substitute(
      "http://$0/jsonmetricz?metrics=$1",
      hostport.ToString(),
      metric_proto_name);
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
    if (type != entity_proto_name) {
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
      if (name != metric_proto_name) {
        continue;
      }
      int64_t value;
      RETURN_NOT_OK(r.ExtractInt64(metric, value_field, &value));
      return value;
    }
  }
  string msg;
  if (entity_id) {
    msg = Substitute("Could not find metric $0.$1 for entity $2",
                     entity_proto_name, metric_proto_name,
                     entity_id);
  } else {
    msg = Substitute("Could not find metric $0.$1",
                     entity_proto_name, metric_proto_name);
  }
  return STATUS(NotFound, msg);
}

string ExternalDaemon::LogPrefix() {
  return Format("{ daemon_id: $0 bound_rpc: $1 } ", daemon_id_, bound_rpc_);
}

void ExternalDaemon::SetLogListener(StringListener* listener) {
  stdout_tailer_thread_->SetListener(listener);
  stderr_tailer_thread_->SetListener(listener);
}

void ExternalDaemon::RemoveLogListener(StringListener* listener) {
  stdout_tailer_thread_->RemoveListener(listener);
  stderr_tailer_thread_->RemoveListener(listener);
}

Result<string> ExternalDaemon::GetFlag(const std::string& flag) {
  server::GenericServiceProxy proxy(proxy_cache_, bound_rpc_addr());

  rpc::RpcController controller;
  controller.set_timeout(MonoDelta::FromSeconds(30));
  server::GetFlagRequestPB req;
  server::GetFlagResponsePB resp;
  req.set_flag(flag);
  RETURN_NOT_OK(proxy.GetFlag(req, &resp, &controller));
  if (!resp.valid()) {
    return STATUS_FORMAT(RemoteError, "Failed to get gflag $0 value.", flag);
  }
  return resp.value();
}

LogWaiter::LogWaiter(ExternalDaemon* daemon, const std::string& string_to_wait) :
    daemon_(daemon), string_to_wait_(string_to_wait) {
  daemon_->SetLogListener(this);
}

void LogWaiter::Handle(const GStringPiece& s) {
  if (s.contains(string_to_wait_)) {
    event_occurred_ = true;
  }
}

Status LogWaiter::WaitFor(const MonoDelta timeout) {
  constexpr auto kInitialWaitPeriod = 100ms;
  return ::yb::WaitFor(
      [this]{ return event_occurred_.load(); }, timeout,
      Format("Waiting for log record '$0' on $1...", string_to_wait_, daemon_->id()),
      kInitialWaitPeriod);
}

LogWaiter::~LogWaiter() {
  daemon_->RemoveLogListener(this);
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
    rpc::Messenger* messenger,
    rpc::ProxyCache* proxy_cache,
    const string& exe,
    const string& data_dir,
    const std::vector<string>& extra_flags,
    const string& rpc_bind_address,
    uint16_t http_port,
    const string& master_addrs)
    : ExternalDaemon(
          Substitute("m-$0", master_index + 1), messenger, proxy_cache, exe, data_dir, "master",
          extra_flags),
      rpc_bind_address_(rpc_bind_address),
      master_addrs_(master_addrs),
      http_port_(http_port) {
}

ExternalMaster::~ExternalMaster() {
}

namespace {

class Flags {
 public:
  template <class Value>
  void Add(const std::string& name, const Value& value) {
    value_.push_back(Format("--$0=$1", name, value));
  }

  void AddHostPort(const std::string& name, const std::string& host, uint16_t port) {
    Add(name, HostPort(host, port));
  }

  const std::vector<std::string>& value() const {
    return value_;
  }

 private:
  std::vector<std::string> value_;
};

} // namespace

Status ExternalMaster::Start(bool shell_mode) {
  Flags flags;
  flags.Add("fs_data_dirs", data_dir_);
  flags.Add("rpc_bind_addresses", rpc_bind_address_);
  flags.Add("webserver_interface", "localhost");
  flags.Add("webserver_port", http_port_);
  // Default master args to make sure we don't wait to trigger new LB tasks upon master leader
  // failover.
  flags.Add("load_balancer_initial_delay_secs", 0);
  // On first start, we need to tell the masters their list of expected peers.
  // For 'shell' master, there is no master addresses.
  if (!shell_mode) {
    flags.Add("master_addresses", master_addrs_);
  }
  RETURN_NOT_OK(StartProcess(flags.value()));
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
    int tablet_server_index, rpc::Messenger* messenger, rpc::ProxyCache* proxy_cache,
    const std::string& exe, const std::string& data_dir, std::string bind_host, uint16_t rpc_port,
    uint16_t http_port, uint16_t redis_rpc_port, uint16_t redis_http_port,
    uint16_t cql_rpc_port, uint16_t cql_http_port,
    uint16_t pgsql_rpc_port, uint16_t pgsql_http_port,
    const std::vector<HostPort>& master_addrs, const std::vector<std::string>& extra_flags)
    : ExternalDaemon(
          Substitute("ts-$0", tablet_server_index + 1), messenger, proxy_cache, exe, data_dir,
          "tserver", extra_flags),
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

Status ExternalTabletServer::Start(bool start_cql_proxy, bool set_proxy_addrs) {
  start_cql_proxy_ = start_cql_proxy;
  Flags flags;
  flags.Add("fs_data_dirs", data_dir_);
  flags.AddHostPort("rpc_bind_addresses", bind_host_, rpc_port_);
  flags.Add("webserver_interface", bind_host_);
  flags.Add("webserver_port", http_port_);
  flags.Add("redis_proxy_webserver_port", redis_http_port_);
  flags.Add("pgsql_proxy_webserver_port", pgsql_http_port_);
  flags.Add("cql_proxy_webserver_port", cql_http_port_);

  if (set_proxy_addrs) {
    flags.AddHostPort("redis_proxy_bind_address", bind_host_, redis_rpc_port_);
    flags.AddHostPort("pgsql_proxy_bind_address", bind_host_, pgsql_rpc_port_);
    flags.AddHostPort("cql_proxy_bind_address", bind_host_, cql_rpc_port_);
  }

  flags.Add("start_cql_proxy", start_cql_proxy_);
  flags.Add("tserver_master_addrs", master_addrs_);

  // Use conservative number of threads for the mini cluster for unit test env
  // where several unit tests tend to run in parallel.
  flags.Add("tablet_server_svc_num_threads", "64");
  flags.Add("ts_consensus_svc_num_threads", "20");

  RETURN_NOT_OK(StartProcess(flags.value()));

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
