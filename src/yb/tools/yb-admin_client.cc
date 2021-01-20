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
#include "yb/tools/yb-admin_client.h"

#include <array>
#include <iomanip>
#include <sstream>
#include <type_traits>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/global_fun.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/tti/has_member_function.hpp>

#include <google/protobuf/util/json_util.h>

#include "yb/common/redis_constants_common.h"
#include "yb/common/wire_protocol.h"
#include "yb/client/client.h"
#include "yb/client/table_alterer.h"
#include "yb/client/table_creator.h"
#include "yb/master/master.pb.h"
#include "yb/master/master_error.h"
#include "yb/master/sys_catalog.h"
#include "yb/rpc/messenger.h"

#include "yb/util/string_case.h"
#include "yb/util/net/net_util.h"
#include "yb/util/string_util.h"
#include "yb/util/protobuf_util.h"
#include "yb/util/random_util.h"
#include "yb/gutil/strings/split.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/numbers.h"

#include "yb/consensus/consensus.proxy.h"
#include "yb/tserver/tserver_service.proxy.h"

DEFINE_bool(wait_if_no_leader_master, false,
            "When yb-admin connects to the cluster and no leader master is present, "
            "this flag determines if yb-admin should wait for the entire duration of timeout or"
            "in case a leader master appears in that duration or return error immediately.");

DEFINE_string(certs_dir_name, "",
              "Directory with certificates to use for secure server connection.");

DEFINE_bool(
    disable_graceful_transition, false,
    "During a leader stepdown, disable graceful leadership transfer "
    "to an up to date peer");

// Maximum number of elements to dump on unexpected errors.
static constexpr int MAX_NUM_ELEMENTS_TO_SHOW_ON_ERROR = 10;

PB_ENUM_FORMATTERS(yb::consensus::RaftPeerPB::Role);
PB_ENUM_FORMATTERS(yb::AppStatusPB::ErrorCode);
PB_ENUM_FORMATTERS(yb::tablet::RaftGroupStatePB);

namespace yb {
namespace tools {

using namespace std::literals;

using std::cout;
using std::endl;

using google::protobuf::RepeatedPtrField;
using google::protobuf::util::MessageToJsonString;

using client::YBClientBuilder;
using client::YBTableName;
using rpc::MessengerBuilder;
using rpc::RpcController;
using strings::Substitute;
using tserver::TabletServerServiceProxy;

using consensus::ConsensusServiceProxy;
using consensus::LeaderStepDownRequestPB;
using consensus::LeaderStepDownResponsePB;
using consensus::RaftPeerPB;
using consensus::RunLeaderElectionRequestPB;
using consensus::RunLeaderElectionResponsePB;

using master::ListMastersRequestPB;
using master::ListMastersResponsePB;
using master::ListMasterRaftPeersRequestPB;
using master::ListMasterRaftPeersResponsePB;
using master::ListTabletServersRequestPB;
using master::ListTabletServersResponsePB;
using master::MasterServiceProxy;
using master::TabletLocationsPB;
using master::TSInfoPB;

namespace {

static constexpr const char* kRpcHostPortHeading = "RPC Host/Port";
static constexpr const char* kDBTypePrefixUnknown = "unknown";
static constexpr const char* kDBTypePrefixCql = "ycql";
static constexpr const char* kDBTypePrefixYsql = "ysql";
static constexpr const char* kDBTypePrefixRedis = "yedis";
static constexpr const char* kTableIDPrefix = "tableid";

string FormatFirstHostPort(
    const RepeatedPtrField<HostPortPB>& rpc_addresses) {
  if (rpc_addresses.empty()) {
    return "N/A";
  } else {
    return HostPortPBToString(rpc_addresses.Get(0));
  }
}

string FormatDouble(double d, int precision = 2) {
  std::ostringstream op_stream;
  op_stream << std::fixed << std::setprecision(precision);
  op_stream << d;
  return op_stream.str();
}

const int kPartitionRangeColWidth = 56;
const int kHostPortColWidth = 20;
const int kTableNameColWidth = 48;
const int kNumCharactersInUuid = 32;
const int kLongColWidth = 15;
const int kSmallColWidth = 8;
const int kSleepTimeSec = 1;
const int kNumberOfTryouts = 30;

BOOST_TTI_HAS_MEMBER_FUNCTION(has_error)
template<typename T>
constexpr bool HasMemberFunctionHasError = has_member_function_has_error<const T, bool>::value;

template<class Response>
Result<Response> ResponseResult(Response&& response,
    typename std::enable_if<HasMemberFunctionHasError<Response>, void*>::type = nullptr) {
  // Response has has_error method, use status from it
  if(response.has_error()) {
    return StatusFromPB(response.error().status());
  }
  return std::move(response);
}

template<class Response>
Result<Response> ResponseResult(Response&& response,
    typename std::enable_if<!HasMemberFunctionHasError<Response>, void*>::type = nullptr) {
  // Response has no has_error method, nothing to check
  return std::move(response);
}

const char* DatabasePrefix(YQLDatabase db) {
  switch(db) {
    case YQL_DATABASE_UNKNOWN: break;
    case YQL_DATABASE_CQL: return kDBTypePrefixCql;
    case YQL_DATABASE_PGSQL: return kDBTypePrefixYsql;
    case YQL_DATABASE_REDIS: return kDBTypePrefixRedis;
  }
  CHECK(false) << "Unexpected db type " << db;
  return kDBTypePrefixUnknown;
}

Result<TypedNamespaceName> ResolveNamespaceName(
    const Slice& prefix,
    const Slice& name,
    const YQLDatabase default_if_no_prefix = YQL_DATABASE_CQL) {
  auto db_type = YQL_DATABASE_UNKNOWN;
  if (!prefix.empty()) {
    static const std::array<pair<const char*, YQLDatabase>, 3> type_prefixes{
        make_pair(kDBTypePrefixCql, YQL_DATABASE_CQL),
        make_pair(kDBTypePrefixYsql, YQL_DATABASE_PGSQL),
        make_pair(kDBTypePrefixRedis, YQL_DATABASE_REDIS)};
    for (const auto& p : type_prefixes) {
      if (prefix == p.first) {
        db_type = p.second;
        break;
      }
    }

    if (db_type == YQL_DATABASE_UNKNOWN) {
      return STATUS_FORMAT(InvalidArgument, "Invalid db type name '$0'", prefix);
    }
  } else {
    db_type = (name == common::kRedisKeyspaceName ? YQL_DATABASE_REDIS : default_if_no_prefix);
  }
  return TypedNamespaceName{.db_type = db_type, .name = name.cdata()};
}

Slice GetTableIdAsSlice(const YBTableName& table_name) {
  return table_name.table_id();
}

Slice GetNamespaceIdAsSlice(const YBTableName& table_name) {
  return table_name.namespace_id();
}

Slice GetTableNameAsSlice(const YBTableName& table_name) {
  return table_name.table_name();
}

std::string FullNamespaceName(const master::NamespaceIdentifierPB& ns) {
  return Format("$0.$1", DatabasePrefix(ns.database_type()), ns.name());
}

struct NamespaceKey {
  explicit NamespaceKey(const master::NamespaceIdentifierPB& ns)
      : db_type(ns.database_type()), name(ns.name()) {
  }

  NamespaceKey(YQLDatabase d, const Slice& n)
      : db_type(d), name(n) {
  }

  YQLDatabase db_type;
  Slice name;
};

struct NamespaceComparator {
  using is_transparent = void;

  bool operator()(const master::NamespaceIdentifierPB& lhs,
                  const master::NamespaceIdentifierPB& rhs) const {
    return (*this)(NamespaceKey(lhs), NamespaceKey(rhs));
  }

  bool operator()(const master::NamespaceIdentifierPB& lhs, const NamespaceKey& rhs) const {
    return (*this)(NamespaceKey(lhs), rhs);
  }

  bool operator()(const NamespaceKey& lhs, const master::NamespaceIdentifierPB& rhs) const {
    return (*this)(lhs, NamespaceKey(rhs));
  }

  bool operator()(const NamespaceKey& lhs, const NamespaceKey& rhs) const {
    return lhs.db_type < rhs.db_type ||
           (lhs.db_type == rhs.db_type && lhs.name.compare(rhs.name) < 0);
  }
};

struct DotStringParts {
  Slice prefix;
  Slice value;
};

DotStringParts SplitByDot(const std::string& str) {
  const size_t dot_pos = str.find('.');
  DotStringParts result{.prefix = Slice(), .value = str};
  if (dot_pos != string::npos) {
    result.prefix = Slice(str.data(), dot_pos);
    result.value.remove_prefix(dot_pos + 1);
  }
  return result;
}

}  // anonymous namespace

class TableNameResolver::Impl {
 public:
  struct TableIdTag;
  struct TableNameTag;
  using Values = std::vector<client::YBTableName>;

  Impl(std::vector<YBTableName> tables, vector<master::NamespaceIdentifierPB> namespaces)
      : current_namespace_(nullptr) {
    std::move(tables.begin(), tables.end(), std::inserter(tables_, tables_.end()));
    std::move(namespaces.begin(), namespaces.end(), std::inserter(namespaces_, namespaces_.end()));
  }

  Result<bool> Feed(const std::string& str) {
    const auto result = FeedImpl(str);
    if (!result.ok()) {
      current_namespace_ = nullptr;
    }
    return result;
  }

  Values& values() {
    return values_;
  }

 private:
  Result<bool> FeedImpl(const std::string& str) {
    auto parts = SplitByDot(str);
    if (parts.prefix == kTableIDPrefix) {
      RETURN_NOT_OK(ProcessTableId(parts.value));
      return true;
    } else {
      if (!current_namespace_) {
        RETURN_NOT_OK(ProcessNamespace(parts.prefix, parts.value));
      } else {
        if (parts.prefix.empty()) {
          RETURN_NOT_OK(ProcessTableName(parts.value));
          return true;
        }
        return STATUS(InvalidArgument, "Wrong table name " + str);
      }
    }
    return false;
  }

  CHECKED_STATUS ProcessNamespace(const Slice& prefix, const Slice& value) {
    DCHECK(!current_namespace_);
    const auto ns = VERIFY_RESULT(ResolveNamespaceName(prefix, value));
    const auto i = namespaces_.find(NamespaceKey(ns.db_type, ns.name));
    if (i != namespaces_.end()) {
      current_namespace_ = &*i;
      return Status::OK();
    }
    return STATUS_FORMAT(
        InvalidArgument, "Namespace '$0' of type '$1' not found",
        ns.name, DatabasePrefix(ns.db_type));
  }

  CHECKED_STATUS ProcessTableId(const Slice& table_id) {
    const auto& idx = tables_.get<TableIdTag>();
    const auto i = idx.find(table_id);
    if (i == idx.end()) {
      return STATUS_FORMAT(InvalidArgument, "Table with id '$0' not found", table_id);
    }
    if (current_namespace_ && current_namespace_->id() != i->namespace_id()) {
      return STATUS_FORMAT(
          InvalidArgument, "Table with id '$0' belongs to different namespace '$1'",
          table_id, FullNamespaceName(*current_namespace_));
    }
    AppendTable(*i);
    return Status::OK();
  }

  CHECKED_STATUS ProcessTableName(const Slice& table_name) {
    DCHECK(current_namespace_);
    const auto& idx = tables_.get<TableNameTag>();
    const auto key = boost::make_tuple(Slice(current_namespace_->id()), table_name);
    // For some reason idx.equal_range(key) failed to compile.
    const auto range = std::make_pair(idx.lower_bound(key), idx.upper_bound(key));
    switch (std::distance(range.first, range.second)) {
      case 0:
        return STATUS_FORMAT(
            InvalidArgument, "Table with name '$0' not found in namespace '$1'",
            table_name, FullNamespaceName(*current_namespace_));
      case 1:
        AppendTable(*range.first);
        return Status::OK();
      default:
        return STATUS_FORMAT(
            InvalidArgument,
            "Namespace '$0' has multiple tables named '$1', specify table id instead",
            FullNamespaceName(*current_namespace_), table_name);
    }
  }

  void AppendTable(const YBTableName& table) {
    current_namespace_ = nullptr;
    values_.push_back(table);
  }

  using TableContainer = boost::multi_index_container<YBTableName,
      boost::multi_index::indexed_by<
          boost::multi_index::ordered_unique<
              boost::multi_index::tag<TableIdTag>,
              boost::multi_index::global_fun<const YBTableName&, Slice, &GetTableIdAsSlice>,
              Slice::Comparator
          >,
          boost::multi_index::ordered_non_unique<
              boost::multi_index::tag<TableNameTag>,
              boost::multi_index::composite_key<
                  YBTableName,
                  boost::multi_index::global_fun<const YBTableName&, Slice, &GetNamespaceIdAsSlice>,
                  boost::multi_index::global_fun<const YBTableName&, Slice, &GetTableNameAsSlice>
              >,
              boost::multi_index::composite_key_compare<
                  Slice::Comparator,
                  Slice::Comparator
              >
          >
      >
  >;

  TableContainer tables_;
  std::set<master::NamespaceIdentifierPB, NamespaceComparator> namespaces_;
  const master::NamespaceIdentifierPB* current_namespace_;
  Values values_;
};

TableNameResolver::TableNameResolver(std::vector<client::YBTableName> tables,
                                     std::vector<master::NamespaceIdentifierPB> namespaces)
    : impl_(new Impl(std::move(tables), std::move(namespaces))) {
}

TableNameResolver::TableNameResolver(TableNameResolver&&) = default;

TableNameResolver::~TableNameResolver() = default;

Result<bool> TableNameResolver::Feed(const std::string& value) {
  return impl_->Feed(value);
}

std::vector<client::YBTableName>& TableNameResolver::values() {
  return impl_->values();
}

ClusterAdminClient::ClusterAdminClient(string addrs, MonoDelta timeout)
    : master_addr_list_(std::move(addrs)),
      timeout_(timeout),
      initted_(false) {}

ClusterAdminClient::ClusterAdminClient(const HostPort& init_master_addr, MonoDelta timeout)
    : init_master_addr_(init_master_addr),
      timeout_(timeout),
      initted_(false) {}

ClusterAdminClient::~ClusterAdminClient() {
  if (messenger_) {
    messenger_->Shutdown();
  }
}

Status ClusterAdminClient::DiscoverAllMasters(
    const HostPort& init_master_addr,
    std::string* all_master_addrs
) {

  std::unique_ptr<MasterServiceProxy> master_proxy(new MasterServiceProxy(
      proxy_cache_.get(),
      init_master_addr));

  VLOG(0) << "Initializing master leader list from single master at "
          << init_master_addr.ToString();
  const auto list_resp = VERIFY_RESULT(InvokeRpc(&MasterServiceProxy::ListMasters,
      master_proxy.get(), ListMastersRequestPB()));
  if (list_resp.masters().empty()) {
    return  STATUS(NotFound, "no masters found");
  }

  std::vector<std::string> addrs;
  for (const auto& master : list_resp.masters()) {
    if (!master.has_registration()) {
      LOG(WARNING) << master.instance_id().permanent_uuid() << " has no registration.";
      continue;
    }

    if (master.registration().broadcast_addresses_size() > 0) {
      addrs.push_back(FormatFirstHostPort(master.registration().broadcast_addresses()));
    } else if (master.registration().private_rpc_addresses_size() > 0) {
      addrs.push_back(FormatFirstHostPort(master.registration().private_rpc_addresses()));
    } else {
      LOG(WARNING) << master.instance_id().permanent_uuid() << " has no rpc/broadcast address.";
      continue;
    }
  }

  if (addrs.empty()) {
    return STATUS(NotFound, "no masters found");
  }

  JoinStrings(addrs, ",", all_master_addrs);
  VLOG(0) << "Discovered full master list: " << *all_master_addrs;
  return Status::OK();
}

Status ClusterAdminClient::Init() {
  CHECK(!initted_);

  // Check if caller will initialize the client and related parts.
  rpc::MessengerBuilder messenger_builder("yb-admin");
  if (!FLAGS_certs_dir_name.empty()) {
    LOG(INFO) << "Built secure client using certs dir " << FLAGS_certs_dir_name;
    secure_context_ = VERIFY_RESULT(server::CreateSecureContext(FLAGS_certs_dir_name));
    server::ApplySecureContext(secure_context_.get(), &messenger_builder);
  }

  messenger_ = VERIFY_RESULT(messenger_builder.Build());
  proxy_cache_ = std::make_unique<rpc::ProxyCache>(messenger_.get());

  if (!init_master_addr_.host().empty()) {
    RETURN_NOT_OK(DiscoverAllMasters(init_master_addr_, &master_addr_list_));
  }

  yb_client_ = VERIFY_RESULT(YBClientBuilder()
      .add_master_server_addr(master_addr_list_)
      .default_admin_operation_timeout(timeout_)
      .wait_for_leader_election_on_init(FLAGS_wait_if_no_leader_master)
      .Build(messenger_.get()));

  // Find the leader master's socket info to set up the master proxy.
  leader_addr_ = yb_client_->GetMasterLeaderAddress();
  master_proxy_.reset(new MasterServiceProxy(proxy_cache_.get(), leader_addr_));

  rpc::ProxyCache proxy_cache(messenger_.get());
  master_backup_proxy_.reset(new master::MasterBackupServiceProxy(&proxy_cache, leader_addr_));

  initted_ = true;
  return Status::OK();
}

Status ClusterAdminClient::MasterLeaderStepDown(
    const string& leader_uuid,
    const string& new_leader_uuid) {
  auto master_proxy = std::make_unique<ConsensusServiceProxy>(proxy_cache_.get(), leader_addr_);

  return LeaderStepDown(leader_uuid, yb::master::kSysCatalogTabletId,
      new_leader_uuid, &master_proxy);
}

CHECKED_STATUS ClusterAdminClient::LeaderStepDownWithNewLeader(
    const std::string& tablet_id,
    const std::string& dest_ts_uuid) {
  return LeaderStepDown(
      /* leader_uuid */ std::string(),
      tablet_id,
      dest_ts_uuid,
      /* leader_proxy */ nullptr);
}

Status ClusterAdminClient::LeaderStepDown(
    const PeerId& leader_uuid,
    const TabletId& tablet_id,
    const PeerId& new_leader_uuid,
    std::unique_ptr<ConsensusServiceProxy>* leader_proxy) {
  LeaderStepDownRequestPB req;
  req.set_tablet_id(tablet_id);
  if (!new_leader_uuid.empty()) {
    req.set_new_leader_uuid(new_leader_uuid);
  } else {
    if (FLAGS_disable_graceful_transition) {
      req.set_disable_graceful_transition(true);
    }
  }
  // The API for InvokeRpcNoResponseCheck requires a raw pointer to a ConsensusServiceProxy, so
  // cache it outside, if we are creating a new proxy to a previously unknown leader.
  std::unique_ptr<ConsensusServiceProxy> new_proxy;
  if (!leader_uuid.empty()) {
    // TODO: validate leader_proxy ?
    req.set_dest_uuid(leader_uuid);
  } else {
    // Look up the location of the tablet leader from the Master.
    HostPort leader_addr;
    string lookup_leader_uuid;
    RETURN_NOT_OK(SetTabletPeerInfo(tablet_id, LEADER, &lookup_leader_uuid, &leader_addr));
    req.set_dest_uuid(lookup_leader_uuid);
    new_proxy = std::make_unique<ConsensusServiceProxy>(proxy_cache_.get(), leader_addr);
  }
  VLOG(2) << "Sending request " << req.DebugString() << " to node with uuid [" << leader_uuid
          << "]";
  const auto resp = VERIFY_RESULT(InvokeRpcNoResponseCheck(&ConsensusServiceProxy::LeaderStepDown,
      new_proxy ? new_proxy.get() : leader_proxy->get(),
      req));
  if (resp.has_error()) {
    LOG(ERROR) << "LeaderStepDown for " << leader_uuid << "received error "
      << resp.error().ShortDebugString();
    return StatusFromPB(resp.error().status());
  }
  return Status::OK();
}

// Force start an election on a randomly chosen non-leader peer of this tablet's raft quorum.
Status ClusterAdminClient::StartElection(const TabletId& tablet_id) {
  HostPort non_leader_addr;
  string non_leader_uuid;
  RETURN_NOT_OK(SetTabletPeerInfo(tablet_id, FOLLOWER, &non_leader_uuid, &non_leader_addr));
  ConsensusServiceProxy non_leader_proxy(proxy_cache_.get(), non_leader_addr);
  RunLeaderElectionRequestPB req;
  req.set_dest_uuid(non_leader_uuid);
  req.set_tablet_id(tablet_id);
  return ResultToStatus(InvokeRpc(&ConsensusServiceProxy::RunLeaderElection,
      &non_leader_proxy, req));
}

// Look up the location of the tablet server leader or non-leader peer from the leader master
Status ClusterAdminClient::SetTabletPeerInfo(
    const TabletId& tablet_id,
    PeerMode mode,
    PeerId* peer_uuid,
    HostPort* peer_addr) {
  TSInfoPB peer_ts_info;
  RETURN_NOT_OK(GetTabletPeer(tablet_id, mode, &peer_ts_info));
  auto rpc_addresses = peer_ts_info.private_rpc_addresses();
  CHECK_GT(rpc_addresses.size(), 0) << peer_ts_info
        .ShortDebugString();

  *peer_addr = HostPortFromPB(rpc_addresses.Get(0));
  *peer_uuid = peer_ts_info.permanent_uuid();
  return Status::OK();
}

Status ClusterAdminClient::ParseChangeType(
    const string& change_type,
    consensus::ChangeConfigType* cc_type) {
  consensus::ChangeConfigType cctype = consensus::UNKNOWN_CHANGE;
  *cc_type = cctype;
  string uppercase_change_type;
  ToUpperCase(change_type, &uppercase_change_type);
  if (!consensus::ChangeConfigType_Parse(uppercase_change_type, &cctype) ||
    cctype == consensus::UNKNOWN_CHANGE) {
    return STATUS(InvalidArgument, "Unsupported change_type", change_type);
  }

  *cc_type = cctype;

  return Status::OK();
}

Status ClusterAdminClient::ChangeConfig(
    const TabletId& tablet_id,
    const string& change_type,
    const PeerId& peer_uuid,
    const boost::optional<string>& member_type) {
  CHECK(initted_);

  consensus::ChangeConfigType cc_type;
  RETURN_NOT_OK(ParseChangeType(change_type, &cc_type));

  RaftPeerPB peer_pb;
  peer_pb.set_permanent_uuid(peer_uuid);

  // Parse the optional fields.
  if (member_type) {
    RaftPeerPB::MemberType member_type_val;
    string uppercase_member_type;
    ToUpperCase(*member_type, &uppercase_member_type);
    if (!RaftPeerPB::MemberType_Parse(uppercase_member_type, &member_type_val)) {
      return STATUS(InvalidArgument, "Unrecognized member_type", *member_type);
    }
    if (member_type_val != RaftPeerPB::PRE_VOTER && member_type_val != RaftPeerPB::PRE_OBSERVER) {
      return STATUS(InvalidArgument, "member_type should be PRE_VOTER or PRE_OBSERVER");
    }
    peer_pb.set_member_type(member_type_val);
  }

  // Validate the existence of the optional fields.
  if (!member_type && cc_type == consensus::ADD_SERVER) {
    return STATUS(InvalidArgument, "Must specify member_type when adding a server.");
  }

  // Look up RPC address of peer if adding as a new server.
  if (cc_type == consensus::ADD_SERVER) {
    HostPort host_port = VERIFY_RESULT(GetFirstRpcAddressForTS(peer_uuid));
    HostPortToPB(host_port, peer_pb.mutable_last_known_private_addr()->Add());
  }

  // Look up the location of the tablet leader from the Master.
  HostPort leader_addr;
  string leader_uuid;
  RETURN_NOT_OK(SetTabletPeerInfo(tablet_id, LEADER, &leader_uuid, &leader_addr));

  auto consensus_proxy = std::make_unique<ConsensusServiceProxy>(proxy_cache_.get(), leader_addr);
  // If removing the leader ts, then first make it step down and that
  // starts an election and gets a new leader ts.
  if (cc_type == consensus::REMOVE_SERVER &&
      leader_uuid == peer_uuid) {
    string old_leader_uuid = leader_uuid;
    RETURN_NOT_OK(LeaderStepDown(
          leader_uuid, tablet_id, /* new_leader_uuid */ std::string(), &consensus_proxy));
    sleep(5);  // TODO - election completion timing is not known accurately
    RETURN_NOT_OK(SetTabletPeerInfo(tablet_id, LEADER, &leader_uuid, &leader_addr));
    if (leader_uuid != old_leader_uuid) {
      return STATUS(ConfigurationError,
                    "Old tablet server leader same as new even after re-election!");
    }
    consensus_proxy.reset(new ConsensusServiceProxy(proxy_cache_.get(), leader_addr));
  }

  consensus::ChangeConfigRequestPB req;
  req.set_dest_uuid(leader_uuid);
  req.set_tablet_id(tablet_id);
  req.set_type(cc_type);
  *req.mutable_server() = peer_pb;
  return ResultToStatus(InvokeRpc(&ConsensusServiceProxy::ChangeConfig,
      consensus_proxy.get(), req));
}

Result<std::string> ClusterAdminClient::GetMasterLeaderUuid() {
  std::string leader_uuid;
  const auto list_resp = VERIFY_RESULT_PREPEND(
      InvokeRpc(&MasterServiceProxy::ListMasters, master_proxy_.get(), ListMastersRequestPB()),
      "Could not locate master leader");
  for (const auto& master : list_resp.masters()) {
    if (master.role() == RaftPeerPB::LEADER) {
      SCHECK(
          leader_uuid.empty(), ConfigurationError, "Found two LEADER's in the same raft config.");
      leader_uuid = master.instance_id().permanent_uuid();
    }
  }
  SCHECK(!leader_uuid.empty(), ConfigurationError, "Could not locate master leader!");
  return std::move(leader_uuid);
}

Status ClusterAdminClient::DumpMasterState(bool to_console) {
  CHECK(initted_);
  master::DumpMasterStateRequestPB req;
  req.set_peers_also(true);
  req.set_on_disk(true);
  req.set_return_dump_as_string(to_console);

  const auto resp = VERIFY_RESULT(InvokeRpc(
      &MasterServiceProxy::DumpState, master_proxy_.get(), req));

  if (to_console) {
    cout << resp.dump() << endl;
  } else {
    cout << "Master state dump has been completed and saved into "
            "the master respective log files." << endl;
  }
  return Status::OK();
}

Status ClusterAdminClient::GetLoadMoveCompletion() {
  CHECK(initted_);
  const auto resp = VERIFY_RESULT(InvokeRpc(
      &MasterServiceProxy::GetLoadMoveCompletion, master_proxy_.get(),
      master::GetLoadMovePercentRequestPB()));
  cout << "Percent complete = " << resp.percent() << " : "
    << resp.remaining() << " remaining out of " << resp.total() << endl;
  return Status::OK();
}

Status ClusterAdminClient::GetLeaderBlacklistCompletion() {
  CHECK(initted_);
  const auto resp = VERIFY_RESULT(InvokeRpc(
      &MasterServiceProxy::GetLeaderBlacklistCompletion, master_proxy_.get(),
      master::GetLeaderBlacklistPercentRequestPB()));
  cout << "Percent complete = " << resp.percent() << " : "
    << resp.remaining() << " remaining out of " << resp.total() << endl;
  return Status::OK();
}

Status ClusterAdminClient::GetIsLoadBalancerIdle() {
  CHECK(initted_);

  const bool is_idle = VERIFY_RESULT(yb_client_->IsLoadBalancerIdle());
  cout << "Idle = " << is_idle << endl;
  return Status::OK();
}

Status ClusterAdminClient::ListLeaderCounts(const YBTableName& table_name) {
  unordered_map<string, int> leader_counts = VERIFY_RESULT(GetLeaderCounts(table_name));
  int total_leader_count = 0;
  for (const auto& lc : leader_counts) { total_leader_count += lc.second; }

  // Calculate the standard deviation and adjusted deviation percentage according to the best and
  // worst-case scenarios. Best-case distribution is when leaders are evenly distributed and
  // worst-case is when leaders are all on one tablet server.
  // For example, say we have 16 leaders on 3 tablet servers:
  //   Leader distribution:    7 5 4
  //   Best-case scenario:     6 5 5
  //   Worst-case scenario:   12 0 0
  //   Standard deviation:    1.24722
  //   Adjusted deviation %:  10.9717%
  vector<double> leader_dist, best_case, worst_case;
  cout << RightPadToUuidWidth("Server UUID") << kColumnSep << "Leader Count" << endl;
  for (const auto& leader_count : leader_counts) {
    cout << leader_count.first << kColumnSep << leader_count.second << endl;
    leader_dist.push_back(leader_count.second);
  }

  if (!leader_dist.empty()) {
    for (int i = 0; i < leader_dist.size(); ++i) {
      best_case.push_back(total_leader_count / leader_dist.size());
      worst_case.push_back(0);
    }
    for (int i = 0; i < total_leader_count % leader_dist.size(); ++i) {
      ++best_case[i];
    }
    worst_case[0] = total_leader_count;

    double stdev = yb::standard_deviation(leader_dist);
    double best_stdev = yb::standard_deviation(best_case);
    double worst_stdev = yb::standard_deviation(worst_case);
    double percent_dev = (stdev - best_stdev) / (worst_stdev - best_stdev) * 100.0;
    cout << "Standard deviation: " << stdev << endl;
    cout << "Adjusted deviation percentage: " << percent_dev << "%" << endl;
  }

  return Status::OK();
}

Result<unordered_map<string, int>> ClusterAdminClient::GetLeaderCounts(
    const client::YBTableName& table_name) {
  vector<string> tablet_ids, ranges;
  RETURN_NOT_OK(yb_client_->GetTablets(table_name, 0, &tablet_ids, &ranges));
  master::GetTabletLocationsRequestPB req;
  for (const auto& tablet_id : tablet_ids) {
    req.add_tablet_ids(tablet_id);
  }
  const auto resp = VERIFY_RESULT(InvokeRpc(&MasterServiceProxy::GetTabletLocations,
      master_proxy_.get(), req));

  unordered_map<string, int> leader_counts;
  for (const auto& locs : resp.tablet_locations()) {
    for (const auto& replica : locs.replicas()) {
      const auto uuid = replica.ts_info().permanent_uuid();
      switch(replica.role()) {
        case RaftPeerPB::LEADER:
          // If this is a leader, increment leader counts.
          ++leader_counts[uuid];
          break;
        case RaftPeerPB::FOLLOWER:
          // If this is a follower, touch the leader count entry also so that tablet server with
          // followers only and 0 leader will be accounted for still.
          leader_counts[uuid];
          break;
        default:
          break;
      }
    }
  }

  return leader_counts;
}

Status ClusterAdminClient::SetupRedisTable() {
  const YBTableName table_name(
      YQL_DATABASE_REDIS, common::kRedisKeyspaceName, common::kRedisTableName);
  RETURN_NOT_OK(yb_client_->CreateNamespaceIfNotExists(common::kRedisKeyspaceName,
                                                       YQLDatabase::YQL_DATABASE_REDIS));
  // Try to create the table.
  std::unique_ptr<yb::client::YBTableCreator> table_creator(yb_client_->NewTableCreator());
  Status s = table_creator->table_name(table_name)
                              .table_type(yb::client::YBTableType::REDIS_TABLE_TYPE)
                              .Create();
  // If we could create it, then all good!
  if (s.ok()) {
    LOG(INFO) << "Table '" << table_name.ToString() << "' created.";
    // If the table was already there, also not an error...
  } else if (s.IsAlreadyPresent()) {
    LOG(INFO) << "Table '" << table_name.ToString() << "' already exists";
  } else {
    // If any other error, report that!
    LOG(ERROR) << s;
    RETURN_NOT_OK(s);
  }
  return Status::OK();
}

Status ClusterAdminClient::DropRedisTable() {
  const YBTableName table_name(
      YQL_DATABASE_REDIS, common::kRedisKeyspaceName, common::kRedisTableName);
  Status s = yb_client_->DeleteTable(table_name, true /* wait */);
  if (s.ok()) {
    LOG(INFO) << "Table '" << table_name.ToString() << "' deleted.";
  } else if (s.IsNotFound()) {
    LOG(INFO) << "Table '" << table_name.ToString() << "' does not exist.";
  } else {
    RETURN_NOT_OK(s);
  }
  return Status::OK();
}

Status ClusterAdminClient::ChangeMasterConfig(
    const string& change_type,
    const string& peer_host,
    int16 peer_port,
    bool use_hostport) {
  CHECK(initted_);

  VLOG(1) << "ChangeMasterConfig: " << change_type << " | " << peer_host << ":" << peer_port;
  consensus::ChangeConfigType cc_type;
  RETURN_NOT_OK(ParseChangeType(change_type, &cc_type));

  string peer_uuid;
  if (!use_hostport) {
      VLOG(1) << "ChangeMasterConfig: attempt to get UUID for changed host: "
              << peer_host << ":" << peer_port;
      RETURN_NOT_OK(yb_client_->GetMasterUUID(peer_host, peer_port, &peer_uuid));
  }

  auto leader_uuid = VERIFY_RESULT(GetMasterLeaderUuid());

  // If removing the leader master, then first make it step down and that
  // starts an election and gets a new leader master.
  auto changed_leader_addr = leader_addr_;
  if (cc_type == consensus::REMOVE_SERVER && leader_uuid == peer_uuid) {
    VLOG(1) << "ChangeMasterConfig: request leader " << leader_addr_
            << " to step down before removal.";
    string old_leader_uuid = leader_uuid;
    RETURN_NOT_OK(MasterLeaderStepDown(leader_uuid));
    sleep(5);  // TODO - wait for exactly the time needed for new leader to get elected.
    // Reget the leader master's socket info to set up the proxy
    leader_addr_ = VERIFY_RESULT(yb_client_->RefreshMasterLeaderAddress());
    master_proxy_.reset(new MasterServiceProxy(proxy_cache_.get(), leader_addr_));
    leader_uuid = VERIFY_RESULT(GetMasterLeaderUuid());
    if (leader_uuid == old_leader_uuid) {
      return STATUS(ConfigurationError,
        Substitute("Old master leader uuid $0 same as new one even after stepdown!", leader_uuid));
    }
    // Go ahead below and send the actual config change message to the new master
  }

  std::unique_ptr<consensus::ConsensusServiceProxy> leader_proxy(
    new consensus::ConsensusServiceProxy(proxy_cache_.get(), leader_addr_));
  consensus::ChangeConfigRequestPB req;

  RaftPeerPB peer_pb;
  peer_pb.set_permanent_uuid(peer_uuid);
  // Ignored by ChangeConfig if request != ADD_SERVER.
  peer_pb.set_member_type(RaftPeerPB::PRE_VOTER);
  HostPortPB *peer_host_port = peer_pb.mutable_last_known_private_addr()->Add();
  peer_host_port->set_port(peer_port);
  peer_host_port->set_host(peer_host);
  req.set_dest_uuid(leader_uuid);
  req.set_tablet_id(yb::master::kSysCatalogTabletId);
  req.set_type(cc_type);
  req.set_use_host(use_hostport);
  *req.mutable_server() = peer_pb;

  VLOG(1) << "ChangeMasterConfig: ChangeConfig for tablet id " << yb::master::kSysCatalogTabletId
          << " to host " << leader_addr_;
  RETURN_NOT_OK(InvokeRpc(
    &consensus::ConsensusServiceProxy::ChangeConfig,
    leader_proxy.get(),
    req));

  VLOG(1) << "ChangeMasterConfig: update yb client to reflect config change.";
  if (cc_type == consensus::ADD_SERVER) {
    RETURN_NOT_OK(yb_client_->AddMasterToClient(changed_leader_addr));
  } else {
    RETURN_NOT_OK(yb_client_->RemoveMasterFromClient(changed_leader_addr));
  }

  return Status::OK();
}

Status ClusterAdminClient::GetTabletLocations(const TabletId& tablet_id,
                                              TabletLocationsPB* locations) {
  master::GetTabletLocationsRequestPB req;
  req.add_tablet_ids(tablet_id);
  const auto resp = VERIFY_RESULT(InvokeRpc(&MasterServiceProxy::GetTabletLocations,
      master_proxy_.get(), req));

  if (resp.errors_size() > 0) {
    // This tool only needs to support one-by-one requests for tablet
    // locations, so we only look at the first error.
    return StatusFromPB(resp.errors(0).status());
  }

  // Same as above, no batching, and we already got past the error checks.
  CHECK_EQ(1, resp.tablet_locations_size()) << resp.ShortDebugString();

  *locations = resp.tablet_locations(0);
  return Status::OK();
}

Status ClusterAdminClient::GetTabletPeer(const TabletId& tablet_id,
                                         PeerMode mode,
                                         TSInfoPB* ts_info) {
  TabletLocationsPB locations;
  RETURN_NOT_OK(GetTabletLocations(tablet_id, &locations));
  CHECK_EQ(tablet_id, locations.tablet_id()) << locations.ShortDebugString();
  bool found = false;
  for (const TabletLocationsPB::ReplicaPB& replica : locations.replicas()) {
    if (mode == LEADER && replica.role() == RaftPeerPB::LEADER) {
      *ts_info = replica.ts_info();
      found = true;
      break;
    }
    if (mode == FOLLOWER && replica.role() != RaftPeerPB::LEADER) {
      *ts_info = replica.ts_info();
      found = true;
      break;
    }
  }

  if (!found) {
    return STATUS(NotFound,
      Substitute("No peer replica found in $0 mode for tablet $1", mode, tablet_id));
  }

  return Status::OK();
}

Status ClusterAdminClient::ListTabletServers(
    RepeatedPtrField<ListTabletServersResponsePB::Entry>* servers) {
  auto resp = VERIFY_RESULT(InvokeRpc(&MasterServiceProxy::ListTabletServers, master_proxy_.get(),
      ListTabletServersRequestPB()));
  *servers = std::move(*resp.mutable_servers());
  return Status::OK();
}

Result<HostPort> ClusterAdminClient::GetFirstRpcAddressForTS(const PeerId& uuid) {
  RepeatedPtrField<ListTabletServersResponsePB::Entry> servers;
  RETURN_NOT_OK(ListTabletServers(&servers));
  for (const ListTabletServersResponsePB::Entry& server : servers) {
    if (server.instance_id().permanent_uuid() == uuid) {
      if (!server.has_registration() ||
          server.registration().common().private_rpc_addresses().empty()) {
        break;
      }
      return HostPortFromPB(server.registration().common().private_rpc_addresses(0));
    }
  }

  return STATUS_FORMAT(
      NotFound, "Server with UUID $0 has no RPC address registered with the Master", uuid);
}

Status ClusterAdminClient::ListAllTabletServers(bool exclude_dead) {
  RepeatedPtrField<ListTabletServersResponsePB::Entry> servers;
  RETURN_NOT_OK(ListTabletServers(&servers));
  char kSpaceSep = ' ';

  cout << RightPadToUuidWidth("Tablet Server UUID") << kSpaceSep
        << kRpcHostPortHeading << kSpaceSep
        << RightPadToWidth("Heartbeat delay", kLongColWidth) << kSpaceSep
        << RightPadToWidth("Status", kSmallColWidth) << kSpaceSep
        << RightPadToWidth("Reads/s", kSmallColWidth) << kSpaceSep
        << RightPadToWidth("Writes/s", kSmallColWidth) << kSpaceSep
        << RightPadToWidth("Uptime", kSmallColWidth) << kSpaceSep
        << RightPadToWidth("SST total size", kLongColWidth) << kSpaceSep
        << RightPadToWidth("SST uncomp size", kLongColWidth) << kSpaceSep
        << RightPadToWidth("SST #files", kLongColWidth) << kSpaceSep
        << RightPadToWidth("Memory", kSmallColWidth) << kSpaceSep
        << endl;
  for (const ListTabletServersResponsePB::Entry& server : servers) {
    if (exclude_dead && server.has_alive() && !server.alive()) {
      continue;
    }
    std::stringstream time_str;
    auto heartbeat_delay_ms = server.has_millis_since_heartbeat() ?
                               server.millis_since_heartbeat() : 0;
    time_str << std::fixed << std::setprecision(2) << (heartbeat_delay_ms/1000.0) << "s";
    auto status_str = server.has_alive() ? (server.alive() ? "ALIVE" : "DEAD") : "UNKNOWN";
    cout << server.instance_id().permanent_uuid() << kSpaceSep
         << FormatFirstHostPort(server.registration().common().private_rpc_addresses())
         << kSpaceSep
         << RightPadToWidth(time_str.str(), kLongColWidth) << kSpaceSep
         << RightPadToWidth(status_str, kSmallColWidth) << kSpaceSep
         << RightPadToWidth(FormatDouble(server.metrics().read_ops_per_sec()), kSmallColWidth)
         << kSpaceSep
         << RightPadToWidth(FormatDouble(server.metrics().write_ops_per_sec()), kSmallColWidth)
         << kSpaceSep
         << RightPadToWidth(server.metrics().uptime_seconds(), kSmallColWidth) << kSpaceSep
         << RightPadToWidth(HumanizeBytes(server.metrics().total_sst_file_size()), kLongColWidth)
         << kSpaceSep
         << RightPadToWidth(HumanizeBytes(server.metrics().uncompressed_sst_file_size()),
                            kLongColWidth)
         << kSpaceSep
         << RightPadToWidth(server.metrics().num_sst_files(), kLongColWidth) << kSpaceSep
         << RightPadToWidth(HumanizeBytes(server.metrics().total_ram_usage()), kSmallColWidth)
         << kSpaceSep
         << endl;
  }

  return Status::OK();
}

Status ClusterAdminClient::ListAllMasters() {
  const auto lresp = VERIFY_RESULT(InvokeRpc(&MasterServiceProxy::ListMasters,
      master_proxy_.get(), ListMastersRequestPB()));

  if (lresp.has_error()) {
    LOG(ERROR) << "Error: querying leader master for live master info : "
               << lresp.error().DebugString() << endl;
    return STATUS(RemoteError, lresp.error().DebugString());
  }

  cout << RightPadToUuidWidth("Master UUID") << kColumnSep
        << RightPadToWidth(kRpcHostPortHeading, kHostPortColWidth) << kColumnSep
        << RightPadToWidth("State", kSmallColWidth) << kColumnSep
        << "Role" << endl;

  for (const auto& master : lresp.masters()) {
      const auto master_reg = master.has_registration() ? &master.registration() : nullptr;
      cout << (master.has_instance_id() ? master.instance_id().permanent_uuid()
                          : RightPadToUuidWidth("UNKNOWN_UUID")) << kColumnSep;
      cout << RightPadToWidth(
                master_reg ? FormatFirstHostPort(master_reg->private_rpc_addresses())
                            : "UNKNOWN", kHostPortColWidth)
            << kColumnSep;
      cout << RightPadToWidth((master.has_error() ?
                                PBEnumToString(master.error().code()) : "ALIVE"),
                              kSmallColWidth)
            << kColumnSep;
      cout << (master.has_role() ? PBEnumToString(master.role()) : "UNKNOWN") << endl;
  }

  return Status::OK();
}

Status ClusterAdminClient::ListTabletServersLogLocations() {
  RepeatedPtrField<ListTabletServersResponsePB::Entry> servers;
  RETURN_NOT_OK(ListTabletServers(&servers));

  if (!servers.empty()) {
    cout << RightPadToUuidWidth("TS UUID") << kColumnSep
         << kRpcHostPortHeading << kColumnSep
         << "LogLocation"
         << endl;
  }

  for (const ListTabletServersResponsePB::Entry& server : servers) {
    auto ts_uuid = server.instance_id().permanent_uuid();

    HostPort ts_addr = VERIFY_RESULT(GetFirstRpcAddressForTS(ts_uuid));

    TabletServerServiceProxy ts_proxy(proxy_cache_.get(), ts_addr);

    const auto resp = VERIFY_RESULT(InvokeRpc(&TabletServerServiceProxy::GetLogLocation,
        &ts_proxy, tserver::GetLogLocationRequestPB()));
    cout << ts_uuid << kColumnSep
         << ts_addr << kColumnSep
         << resp.log_location() << endl;
  }

  return Status::OK();
}

Status ClusterAdminClient::ListTables(bool include_db_type,
                                      bool include_table_id,
                                      bool include_table_type) {
  const auto tables = VERIFY_RESULT(yb_client_->ListTables());
  const auto& namespace_metadata = VERIFY_RESULT_REF(GetNamespaceMap());
  vector<string> names;
  for (const auto& table : tables) {
    std::stringstream str;
    if (include_db_type) {
      const auto db_type_iter = namespace_metadata.find(table.namespace_id());
      if (db_type_iter != namespace_metadata.end()) {
        str << DatabasePrefix(db_type_iter->second.database_type()) << '.';
      } else {
        LOG(WARNING) << "Table in unknown namespace found " << table.ToString();
        continue;
      }
    }
    str << table.ToString();
    if (include_table_id) {
      str << ' ' << table.table_id();
    }
    if (include_table_type) {
      boost::optional<master::RelationType> relation_type = table.relation_type();
      switch (relation_type.get()) {
        case master::SYSTEM_TABLE_RELATION:
          str << " catalog";
          break;
        case master::USER_TABLE_RELATION:
          str << " table";
          break;
        case master::INDEX_TABLE_RELATION:
          str << " index";
          break;
        default:
          str << " other";
      }
    }
    names.push_back(str.str());
  }
  sort(names.begin(), names.end());
  copy(names.begin(), names.end(), std::ostream_iterator<string>(cout, "\n"));
  return Status::OK();
}

Status ClusterAdminClient::ListTablets(const YBTableName& table_name, int max_tablets) {
  vector<string> tablet_uuids, ranges;
  std::vector<master::TabletLocationsPB> locations;
  RETURN_NOT_OK(yb_client_->GetTablets(
      table_name, max_tablets, &tablet_uuids, &ranges, &locations));
  cout << RightPadToUuidWidth("Tablet-UUID") << kColumnSep
       << RightPadToWidth("Range", kPartitionRangeColWidth) << kColumnSep
       << RightPadToWidth("Leader-IP", kLongColWidth) << kColumnSep << "Leader-UUID" << endl;
  for (int i = 0; i < tablet_uuids.size(); i++) {
    string tablet_uuid = tablet_uuids[i];
    string leader_host_port;
    string leader_uuid;
    const auto& locations_of_this_tablet = locations[i];
    for (const auto& replica : locations_of_this_tablet.replicas()) {
      if (replica.role() == RaftPeerPB::Role::RaftPeerPB_Role_LEADER) {
        if (leader_host_port.empty()) {
          leader_host_port = HostPortPBToString(replica.ts_info().private_rpc_addresses(0));
          leader_uuid = replica.ts_info().permanent_uuid();
        } else {
          LOG(ERROR) << "Multiple leader replicas found for tablet " << tablet_uuid
                     << ": " << locations_of_this_tablet.ShortDebugString();
        }
      }
    }
    cout << tablet_uuid << kColumnSep << RightPadToWidth(ranges[i], kPartitionRangeColWidth)
         << kColumnSep << RightPadToWidth(leader_host_port, kLongColWidth) << kColumnSep
         << leader_uuid << endl;
  }
  return Status::OK();
}

Status ClusterAdminClient::ListPerTabletTabletServers(const TabletId& tablet_id) {
  master::GetTabletLocationsRequestPB req;
  req.add_tablet_ids(tablet_id);
  const auto resp = VERIFY_RESULT(InvokeRpc(&MasterServiceProxy::GetTabletLocations,
      master_proxy_.get(), req));

  if (resp.tablet_locations_size() != 1) {
    if (resp.tablet_locations_size() > 0) {
      std::cerr << "List of all incorrect locations - " << resp.tablet_locations_size()
                << " : " << endl;
      const auto limit = std::min(resp.tablet_locations_size(), MAX_NUM_ELEMENTS_TO_SHOW_ON_ERROR);
      for (int i = 0; i < limit; ++i) {
        std::cerr << i << " : " << resp.tablet_locations(i).DebugString();
      }
      std::cerr << endl;
    }
    return STATUS_FORMAT(IllegalState,
                         "Incorrect number of locations $0 for tablet $1.",
                         resp.tablet_locations_size(), tablet_id);
  }

  TabletLocationsPB locs = resp.tablet_locations(0);
  if (!locs.replicas().empty()) {
    cout << RightPadToUuidWidth("Server UUID") << kColumnSep
         << RightPadToWidth(kRpcHostPortHeading, kHostPortColWidth) << kColumnSep
         << "Role" << endl;
  }
  for (const auto& replica : locs.replicas()) {
    cout << replica.ts_info().permanent_uuid() << kColumnSep
         << RightPadToWidth(HostPortPBToString(replica.ts_info().private_rpc_addresses(0)),
                            kHostPortColWidth) << kColumnSep
         << PBEnumToString(replica.role()) << endl;
  }

  return Status::OK();
}

Status ClusterAdminClient::DeleteTable(const YBTableName& table_name) {
  RETURN_NOT_OK(yb_client_->DeleteTable(table_name));
  cout << "Deleted table " << table_name.ToString() << endl;
  return Status::OK();
}

Status ClusterAdminClient::DeleteTableById(const TableId& table_id) {
  RETURN_NOT_OK(yb_client_->DeleteTable(table_id));
  cout << "Deleted table " << table_id << endl;
  return Status::OK();
}

Status ClusterAdminClient::DeleteIndex(const YBTableName& table_name) {
  YBTableName indexed_table_name;
  RETURN_NOT_OK(yb_client_->DeleteIndexTable(table_name, &indexed_table_name));
  cout << "Deleted index " << table_name.ToString() << " from table " <<
      indexed_table_name.ToString() << endl;
  return Status::OK();
}

Status ClusterAdminClient::DeleteIndexById(const TableId& table_id) {
  YBTableName indexed_table_name;
  RETURN_NOT_OK(yb_client_->DeleteIndexTable(table_id, &indexed_table_name));
  cout << "Deleted index " << table_id << " from table " <<
      indexed_table_name.ToString() << endl;
  return Status::OK();
}

Status ClusterAdminClient::DeleteNamespace(const TypedNamespaceName& namespace_name) {
  RETURN_NOT_OK(yb_client_->DeleteNamespace(namespace_name.name, namespace_name.db_type));
  cout << "Deleted namespace " << namespace_name.name << endl;
  return Status::OK();
}

Status ClusterAdminClient::DeleteNamespaceById(const NamespaceId& namespace_id) {
  RETURN_NOT_OK(yb_client_->DeleteNamespace(
      std::string() /* name */, boost::none /* database type */, namespace_id));
  cout << "Deleted namespace " << namespace_id << endl;
  return Status::OK();
}

Status ClusterAdminClient::ListTabletsForTabletServer(const PeerId& ts_uuid) {
  auto ts_addr = VERIFY_RESULT(GetFirstRpcAddressForTS(ts_uuid));

  TabletServerServiceProxy ts_proxy(proxy_cache_.get(), ts_addr);

  const auto resp = VERIFY_RESULT(InvokeRpc(&TabletServerServiceProxy::ListTabletsForTabletServer,
      &ts_proxy, tserver::ListTabletsForTabletServerRequestPB()));

  cout << RightPadToWidth("Table name", kTableNameColWidth) << kColumnSep
       << RightPadToUuidWidth("Tablet ID") << kColumnSep
       << "Is Leader" << kColumnSep
       << "State" << kColumnSep
       << "Num SST Files" << kColumnSep
       << "Num Log Segments" << kColumnSep
       << "Num Memtables (Intents/Regular)" << endl;
  for (const auto& entry : resp.entries()) {
    cout << RightPadToWidth(entry.table_name(), kTableNameColWidth) << kColumnSep
         << RightPadToUuidWidth(entry.tablet_id()) << kColumnSep
         << entry.is_leader() << kColumnSep
         << PBEnumToString(entry.state()) << kColumnSep
         << entry.num_sst_files() << kColumnSep
         << entry.num_log_segments() << kColumnSep
         << entry.num_memtables_intents() << "/" << entry.num_memtables_regular() << endl;
  }
  return Status::OK();
}

Status ClusterAdminClient::SetLoadBalancerEnabled(bool is_enabled) {
  const auto list_resp = VERIFY_RESULT(InvokeRpc(&MasterServiceProxy::ListMasters,
      master_proxy_.get(), ListMastersRequestPB()));

  master::ChangeLoadBalancerStateRequestPB req;
  req.set_is_enabled(is_enabled);
  for (const auto& master : list_resp.masters()) {

    if (master.role() == RaftPeerPB::LEADER) {
      RETURN_NOT_OK(InvokeRpc(&MasterServiceProxy::ChangeLoadBalancerState,
          master_proxy_.get(), req));
    } else {
      HostPortPB hp_pb = master.registration().private_rpc_addresses(0);

      MasterServiceProxy proxy(proxy_cache_.get(), HostPortFromPB(hp_pb));
      RETURN_NOT_OK(InvokeRpc(&MasterServiceProxy::ChangeLoadBalancerState, &proxy, req));
    }
  }

  return Status::OK();
}

Status ClusterAdminClient::GetLoadBalancerState() {
  const auto list_resp = VERIFY_RESULT(InvokeRpc(&MasterServiceProxy::ListMasters,
      master_proxy_.get(), ListMastersRequestPB()));

  if (list_resp.has_error()) {
    LOG(ERROR) << "Error: querying leader master for live master info : "
               << list_resp.error().DebugString() << endl;
    return STATUS(RemoteError, list_resp.error().DebugString());
  }

  cout << RightPadToUuidWidth("Master UUID") << kColumnSep
       << RightPadToWidth(kRpcHostPortHeading, kHostPortColWidth) << kColumnSep
       << RightPadToWidth("State", kSmallColWidth) << kColumnSep
       << RightPadToWidth("Role", kSmallColWidth) << kColumnSep
       << "Load Balancer State" << endl;


  master::GetLoadBalancerStateRequestPB req;
  master::GetLoadBalancerStateResponsePB resp;
  string error;
  MasterServiceProxy* proxy;
  for (const auto& master : list_resp.masters()) {
    error.clear();
    std::unique_ptr<MasterServiceProxy> follower_proxy;
    if (master.role() == RaftPeerPB::LEADER) {
      proxy = master_proxy_.get();
    } else {
      HostPortPB hp_pb = master.registration().private_rpc_addresses(0);
      follower_proxy = std::make_unique<MasterServiceProxy>(
          proxy_cache_.get(), HostPortFromPB(hp_pb));
      proxy = follower_proxy.get();
    }
    auto result = InvokeRpc(&MasterServiceProxy::GetLoadBalancerState, proxy, req);
    if (!result) {
      error = result.ToString();
    } else {
      resp = *result;
      if (!resp.has_error()) {
        error = resp.error().status().message();
      }
    }
    const auto master_reg = master.has_registration() ? &master.registration() : nullptr;
    cout << (master.has_instance_id() ? master.instance_id().permanent_uuid()
                                      : RightPadToUuidWidth("UNKNOWN_UUID")) << kColumnSep;
    cout << RightPadToWidth(
        master_reg ? FormatFirstHostPort(master_reg->private_rpc_addresses())
                   : "UNKNOWN", kHostPortColWidth)
         << kColumnSep;
    cout << RightPadToWidth((master.has_error() ?
                             PBEnumToString(master.error().code()) : "ALIVE"), kSmallColWidth)
         << kColumnSep;
    cout << RightPadToWidth((master.has_role() ?
                             PBEnumToString(master.role()) : "UNKNOWN"), kSmallColWidth)
         << kColumnSep;
    cout << (!error.empty() ? "Error: " + error : (resp.is_enabled() ? "ENABLED" : "DISABLED"))
         << std::endl;
  }

  return Status::OK();
}

Status ClusterAdminClient::FlushTables(const std::vector<YBTableName>& table_names,
                                       bool add_indexes,
                                       int timeout_secs,
                                       bool is_compaction) {
  RETURN_NOT_OK(yb_client_->FlushTables(table_names, add_indexes, timeout_secs, is_compaction));
  cout << (is_compaction ? "Compacted " : "Flushed ")
       << ToString(table_names) << " tables"
       << (add_indexes ? " and associated indexes." : ".") << endl;
  return Status::OK();
}

Status ClusterAdminClient::FlushTablesById(
    const std::vector<TableId>& table_ids,
    bool add_indexes,
    int timeout_secs,
    bool is_compaction) {
  RETURN_NOT_OK(yb_client_->FlushTables(table_ids, add_indexes, timeout_secs, is_compaction));
  cout << (is_compaction ? "Compacted " : "Flushed ")
       << ToString(table_ids) << " tables"
       << (add_indexes ? " and associated indexes." : ".") << endl;
  return Status::OK();
}

Status ClusterAdminClient::WaitUntilMasterLeaderReady() {
  for(int iter = 0; iter < kNumberOfTryouts; ++iter) {
    const auto res_leader_ready = VERIFY_RESULT(InvokeRpcNoResponseCheck(
        &MasterServiceProxy::IsMasterLeaderServiceReady,
        master_proxy_.get(),  master::IsMasterLeaderReadyRequestPB(),
        "MasterServiceImpl::IsMasterLeaderServiceReady call failed."));
    if (!res_leader_ready.has_error()) {
      return Status::OK();
    }
    sleep(kSleepTimeSec);
  }
  return STATUS(TimedOut, "ClusterAdminClient::WaitUntilMasterLeaderReady timed out.");
}

Status ClusterAdminClient::AddReadReplicaPlacementInfo(
    const string& placement_info, int replication_factor, const std::string& optional_uuid) {
  RETURN_NOT_OK_PREPEND(WaitUntilMasterLeaderReady(), "Wait for master leader failed!");

  // Get the cluster config from the master leader.
  auto resp_cluster_config = VERIFY_RESULT(GetMasterClusterConfig());

  auto* cluster_config = resp_cluster_config.mutable_cluster_config();
  if (cluster_config->replication_info().read_replicas_size() > 0) {
    return STATUS(InvalidCommand, "Already have a read replica placement, cannot add another.");
  }
  auto* read_replica_config = cluster_config->mutable_replication_info()->add_read_replicas();

  // If optional_uuid is set, make that the placement info, otherwise generate a random one.
  string uuid_str = optional_uuid;
  if (optional_uuid.empty()) {
    uuid_str = RandomHumanReadableString(16);
  }
  read_replica_config->set_num_replicas(replication_factor);
  read_replica_config->set_placement_uuid(uuid_str);

  // Fill in the placement info with new stuff.
  RETURN_NOT_OK(FillPlacementInfo(read_replica_config, placement_info));

  master::ChangeMasterClusterConfigRequestPB req_new_cluster_config;

  *req_new_cluster_config.mutable_cluster_config() = *cluster_config;

  RETURN_NOT_OK(InvokeRpc(&MasterServiceProxy::ChangeMasterClusterConfig,
                          master_proxy_.get(), req_new_cluster_config,
                          "MasterServiceImpl::ChangeMasterClusterConfig call failed."));

  LOG(INFO) << "Created read replica placement with uuid: " << uuid_str;
  return Status::OK();
}

CHECKED_STATUS ClusterAdminClient::ModifyReadReplicaPlacementInfo(
    const std::string& placement_uuid, const std::string& placement_info, int replication_factor) {
  RETURN_NOT_OK_PREPEND(WaitUntilMasterLeaderReady(), "Wait for master leader failed!");

  // Get the cluster config from the master leader.
  auto master_resp = VERIFY_RESULT(GetMasterClusterConfig());
  auto* cluster_config = master_resp.mutable_cluster_config();

  auto* replication_info = cluster_config->mutable_replication_info();
  if (replication_info->read_replicas_size() == 0) {
    return STATUS(InvalidCommand, "No read replica placement info to modify.");
  }

  auto* read_replica_config = replication_info->mutable_read_replicas(0);

  std::string config_placement_uuid;
  if (placement_uuid.empty())  {
    // If there is no placement_uuid set, use the existing uuid.
    config_placement_uuid = read_replica_config->placement_uuid();
  } else {
    // Otherwise, use the passed in value.
    config_placement_uuid = placement_uuid;
  }

  read_replica_config->Clear();

  read_replica_config->set_num_replicas(replication_factor);
  read_replica_config->set_placement_uuid(config_placement_uuid);
  RETURN_NOT_OK(FillPlacementInfo(read_replica_config, placement_info));

  master::ChangeMasterClusterConfigRequestPB req_new_cluster_config;

  *req_new_cluster_config.mutable_cluster_config() = *cluster_config;

  RETURN_NOT_OK(InvokeRpc(&MasterServiceProxy::ChangeMasterClusterConfig,
                          master_proxy_.get(), req_new_cluster_config,
                          "MasterServiceImpl::ChangeMasterClusterConfig call failed."));

  LOG(INFO) << "Changed read replica placement.";
  return Status::OK();
}

CHECKED_STATUS ClusterAdminClient::DeleteReadReplicaPlacementInfo() {
  RETURN_NOT_OK_PREPEND(WaitUntilMasterLeaderReady(), "Wait for master leader failed!");

  auto master_resp = VERIFY_RESULT(GetMasterClusterConfig());
  auto* cluster_config = master_resp.mutable_cluster_config();

  auto* replication_info = cluster_config->mutable_replication_info();
  if (replication_info->read_replicas_size() == 0) {
    return STATUS(InvalidCommand, "No read replica placement info to delete.");
  }

  replication_info->clear_read_replicas();

  master::ChangeMasterClusterConfigRequestPB req_new_cluster_config;

  *req_new_cluster_config.mutable_cluster_config() = *cluster_config;

  RETURN_NOT_OK(InvokeRpc(&MasterServiceProxy::ChangeMasterClusterConfig,
                          master_proxy_.get(), req_new_cluster_config,
                          "MasterServiceImpl::ChangeMasterClusterConfig call failed."));

  LOG(INFO) << "Deleted read replica placement.";
  return Status::OK();
}

Status ClusterAdminClient::FillPlacementInfo(
    master::PlacementInfoPB* placement_info_pb, const string& placement_str) {

  std::vector<std::string> placement_info_split = strings::Split(
      placement_str, ",", strings::SkipEmpty());
  if (placement_info_split.size() < 1) {
    return STATUS(InvalidCommand, "Cluster config must be a list of "
                                  "placement infos seperated by commas. "
                                  "Format: 'cloud1.region1.zone1:rf,cloud2.region2.zone2:rf, ..."
        + std::to_string(placement_info_split.size()));
  }

  for (int iter = 0; iter < placement_info_split.size(); iter++) {
    std::vector<std::string> placement_block = strings::Split(placement_info_split[iter], ":",
                                                              strings::SkipEmpty());

    if (placement_block.size() != 2) {
      return STATUS(InvalidCommand, "Each placement info must be in format placement:rf");
    }

    int min_num_replicas = boost::lexical_cast<int>(placement_block[1]);

    std::vector<std::string> block = strings::Split(placement_block[0], ".",
                                                    strings::SkipEmpty());
    if (block.size() != 3) {
      return STATUS(InvalidCommand,
          "Each placement info must have exactly 3 values seperated"
          "by dots that denote cloud, region and zone. Block: " + placement_info_split[iter]
          + " is invalid");
    }
    auto pb = placement_info_pb->add_placement_blocks();
    pb->mutable_cloud_info()->set_placement_cloud(block[0]);
    pb->mutable_cloud_info()->set_placement_region(block[1]);
    pb->mutable_cloud_info()->set_placement_zone(block[2]);

    pb->set_min_num_replicas(min_num_replicas);
  }

  return Status::OK();
}

Status ClusterAdminClient::ModifyTablePlacementInfo(
  const YBTableName& table_name, const std::string& placement_info, int replication_factor,
  const std::string& optional_uuid) {

  std::vector<std::string> placement_info_split = strings::Split(
      placement_info, ",", strings::SkipEmpty());
  if (placement_info_split.size() < 1) {
    return STATUS(InvalidCommand, "Table placement config must be a list of "
    "placement infos seperated by commas. "
    "Format: 'cloud1.region1.zone1,cloud2.region2.zone2,cloud3.region3.zone3 ..."
    + std::to_string(placement_info_split.size()));
  }

  master::PlacementInfoPB* live_replicas = new master::PlacementInfoPB;
  live_replicas->set_num_replicas(replication_factor);
  // Iterate over the placement blocks of the placementInfo structure.
  for (int iter = 0; iter < placement_info_split.size(); iter++) {
    std::vector<std::string> block = strings::Split(placement_info_split[iter], ".",
                                                    strings::SkipEmpty());
    if (block.size() != 3) {
      return STATUS(InvalidCommand, "Each placement info must have exactly 3 values seperated"
          "by dots that denote cloud, region and zone. Block: " + placement_info_split[iter]
          + " is invalid");
    }
    auto pb = live_replicas->add_placement_blocks();
    pb->mutable_cloud_info()->set_placement_cloud(block[0]);
    pb->mutable_cloud_info()->set_placement_region(block[1]);
    pb->mutable_cloud_info()->set_placement_zone(block[2]);
    // TODO: Should this also be passed in as input?
    pb->set_min_num_replicas(1);
  }

  if (!optional_uuid.empty()) {
    // If we have an optional uuid, set it.
    live_replicas->set_placement_uuid(optional_uuid);
  }

  master::ReplicationInfoPB replication_info;
  // Merge the obtained info with the existing table replication info.
  std::shared_ptr<client::YBTable> table;
  RETURN_NOT_OK_PREPEND(yb_client_->OpenTable(table_name, &table),
                        "Fetching table schema failed!");

  // If it does not exist, fetch the cluster replication info.
  if (!table->replication_info()) {
    auto resp_cluster_config = VERIFY_RESULT(GetMasterClusterConfig());
    master::SysClusterConfigEntryPB* sys_cluster_config_entry =
      resp_cluster_config.mutable_cluster_config();
    replication_info.CopyFrom(sys_cluster_config_entry->replication_info());
    // TODO(bogdan): Figure out how to handle read replias and leader affinity.
    replication_info.clear_read_replicas();
    replication_info.clear_affinitized_leaders();
  } else {
    // Table replication info exists, copy it over.
    replication_info.CopyFrom(table->replication_info().get());
  }

  // Put in the new live placement info.
  replication_info.set_allocated_live_replicas(live_replicas);

  std::unique_ptr<yb::client::YBTableAlterer> table_alterer(
    yb_client_->NewTableAlterer(table_name));
  return table_alterer->replication_info(replication_info)->Alter();
}

Status ClusterAdminClient::ModifyPlacementInfo(
    std::string placement_info, int replication_factor, const std::string& optional_uuid) {

  // Wait to make sure that master leader is ready.
  RETURN_NOT_OK_PREPEND(WaitUntilMasterLeaderReady(), "Wait for master leader failed!");

  // Get the cluster config from the master leader.
  auto resp_cluster_config = VERIFY_RESULT(GetMasterClusterConfig());

  // Create a new cluster config.
  std::vector<std::string> placement_info_split = strings::Split(
      placement_info, ",", strings::SkipEmpty());
  if (placement_info_split.size() < 1) {
    return STATUS(InvalidCommand, "Cluster config must be a list of "
    "placement infos seperated by commas. "
    "Format: 'cloud1.region1.zone1,cloud2.region2.zone2,cloud3.region3.zone3 ..."
    + std::to_string(placement_info_split.size()));
  }
  master::ChangeMasterClusterConfigRequestPB req_new_cluster_config;
  master::SysClusterConfigEntryPB* sys_cluster_config_entry =
      resp_cluster_config.mutable_cluster_config();
  master::PlacementInfoPB* live_replicas = new master::PlacementInfoPB;
  live_replicas->set_num_replicas(replication_factor);
  // Iterate over the placement blocks of the placementInfo structure.
  for (int iter = 0; iter < placement_info_split.size(); iter++) {
    std::vector<std::string> block = strings::Split(placement_info_split[iter], ".",
                                                    strings::SkipEmpty());
    if (block.size() != 3) {
      return STATUS(InvalidCommand, "Each placement info must have exactly 3 values seperated"
          "by dots that denote cloud, region and zone. Block: " + placement_info_split[iter]
          + " is invalid");
    }
    auto pb = live_replicas->add_placement_blocks();
    pb->mutable_cloud_info()->set_placement_cloud(block[0]);
    pb->mutable_cloud_info()->set_placement_region(block[1]);
    pb->mutable_cloud_info()->set_placement_zone(block[2]);
    pb->set_min_num_replicas(1);
  }

  if (!optional_uuid.empty()) {
    // If we have an optional uuid, set it.
    live_replicas->set_placement_uuid(optional_uuid);
  } else if (sys_cluster_config_entry->replication_info().live_replicas().has_placement_uuid()) {
    // Otherwise, if we have an existing placement uuid, use that.
    live_replicas->set_placement_uuid(
        sys_cluster_config_entry->replication_info().live_replicas().placement_uuid());
  }

  sys_cluster_config_entry->mutable_replication_info()->set_allocated_live_replicas(live_replicas);
  req_new_cluster_config.mutable_cluster_config()->CopyFrom(*sys_cluster_config_entry);

  RETURN_NOT_OK(InvokeRpc(&MasterServiceProxy::ChangeMasterClusterConfig,
      master_proxy_.get(), req_new_cluster_config,
      "MasterServiceImpl::ChangeMasterClusterConfig call failed."));

  LOG(INFO) << "Changed master cluster config.";
  return Status::OK();
}

Status ClusterAdminClient::ClearPlacementInfo() {
  // Wait to make sure that master leader is ready.
  RETURN_NOT_OK_PREPEND(WaitUntilMasterLeaderReady(), "Wait for master leader failed!");

  // Get the cluster config from the master leader.
  auto resp_cluster_config = VERIFY_RESULT(GetMasterClusterConfig());

  master::SysClusterConfigEntryPB* sys_cluster_config_entry =
      resp_cluster_config.mutable_cluster_config();
  sys_cluster_config_entry->clear_replication_info();

  master::ChangeMasterClusterConfigRequestPB req_new_cluster_config;
  req_new_cluster_config.mutable_cluster_config()->CopyFrom(*sys_cluster_config_entry);

  RETURN_NOT_OK(InvokeRpc(&MasterServiceProxy::ChangeMasterClusterConfig,
      master_proxy_.get(), req_new_cluster_config,
      "MasterServiceImpl::ChangeMasterClusterConfig call failed."));

  LOG(INFO) << "Cleared master placement info config";
  return Status::OK();
}

Status ClusterAdminClient::GetUniverseConfig() {
  const auto cluster_config = VERIFY_RESULT(GetMasterClusterConfig());
  std::string output;
  MessageToJsonString(cluster_config.cluster_config(), &output);
  cout << output << endl;
  return Status::OK();
}

Status ClusterAdminClient::GetYsqlCatalogVersion() {
  uint64_t version = 0;
  RETURN_NOT_OK(yb_client_->GetYsqlCatalogMasterVersion(&version));
  cout << "Version: "  << version << endl;
  return Status::OK();
}

Status ClusterAdminClient::ChangeBlacklist(const std::vector<HostPort>& servers, bool add,
    bool blacklist_leader) {
  auto config = VERIFY_RESULT(GetMasterClusterConfig());
  auto& cluster_config = *config.mutable_cluster_config();
  auto& blacklist = (blacklist_leader) ?
    *cluster_config.mutable_leader_blacklist() :
    *cluster_config.mutable_server_blacklist();
  std::vector<HostPort> result_blacklist;
  for (const auto& host : blacklist.hosts()) {
    const HostPort hostport(host.host(), host.port());
    if (std::find(servers.begin(), servers.end(), hostport) == servers.end()) {
      result_blacklist.emplace_back(host.host(), host.port());
    }
  }
  if (add) {
    result_blacklist.insert(result_blacklist.end(), servers.begin(), servers.end());
  }
  auto result_begin = result_blacklist.begin(), result_end = result_blacklist.end();
  std::sort(result_begin, result_end);
  result_blacklist.erase(std::unique(result_begin, result_end), result_end);
  blacklist.clear_hosts();
  for (const auto& hostport : result_blacklist) {
    auto& new_host = *blacklist.add_hosts();
    new_host.set_host(hostport.host());
    new_host.set_port(hostport.port());
  }
  master::ChangeMasterClusterConfigRequestPB req_new_cluster_config;
  req_new_cluster_config.mutable_cluster_config()->Swap(&cluster_config);
  return ResultToStatus(InvokeRpc(&MasterServiceProxy::ChangeMasterClusterConfig,
                                  master_proxy_.get(), req_new_cluster_config,
                                  "MasterServiceImpl::ChangeMasterClusterConfig call failed."));
}

Result<const master::NamespaceIdentifierPB&> ClusterAdminClient::GetNamespaceInfo(
    YQLDatabase db_type, const std::string& namespace_name) {
  LOG(INFO) << Format(
      "Resolving namespace id for '$0' of type '$1'", namespace_name, DatabasePrefix(db_type));
  for (const auto& item : VERIFY_RESULT_REF(GetNamespaceMap())) {
    const auto& namespace_info = item.second;
    if (namespace_info.database_type() == db_type && namespace_name == namespace_info.name()) {
      return namespace_info;
    }
  }
  return STATUS_FORMAT(
      NotFound, "Namespace '$0' of type '$1' not found", namespace_name, DatabasePrefix(db_type));
}

Result<master::GetMasterClusterConfigResponsePB> ClusterAdminClient::GetMasterClusterConfig() {
  return InvokeRpc(&MasterServiceProxy::GetMasterClusterConfig, master_proxy_.get(),
                   master::GetMasterClusterConfigRequestPB(),
                   "MasterServiceImpl::GetMasterClusterConfig call failed.");
}

CHECKED_STATUS ClusterAdminClient::SplitTablet(const std::string& tablet_id) {
  master::SplitTabletRequestPB req;
  req.set_tablet_id(tablet_id);
  const auto resp = VERIFY_RESULT(
      InvokeRpc(&MasterServiceProxy::SplitTablet, master_proxy_.get(), req));
  std::cout << "Response: " << AsString(resp) << std::endl;
  return Status::OK();
}

template<class Response, class Request, class Object>
Result<Response> ClusterAdminClient::InvokeRpcNoResponseCheck(
    Status (Object::*func)(const Request&, Response*, rpc::RpcController*),
    Object* obj, const Request& req, const char* error_message) {
  rpc::RpcController rpc;
  rpc.set_timeout(timeout_);
  Response response;
  auto result = (obj->*func)(req, &response, &rpc);
  if (error_message) {
    RETURN_NOT_OK_PREPEND(result, error_message);
  } else {
    RETURN_NOT_OK(result);
  }
  return std::move(response);
}

template<class Response, class Request, class Object>
Result<Response> ClusterAdminClient::InvokeRpc(
    Status (Object::*func)(const Request&, Response*, rpc::RpcController*),
    Object* obj, const Request& req, const char* error_message) {
  return ResponseResult(VERIFY_RESULT(InvokeRpcNoResponseCheck(func, obj, req, error_message)));
}

Result<const ClusterAdminClient::NamespaceMap&> ClusterAdminClient::GetNamespaceMap() {
  if (namespace_map_.empty()) {
    auto v = VERIFY_RESULT(yb_client_->ListNamespaces());
    for (auto& ns : v) {
      auto ns_id = ns.id();
      namespace_map_.emplace(std::move(ns_id), std::move(ns));
    }
  }
  return const_cast<const ClusterAdminClient::NamespaceMap&>(namespace_map_);
}

Result<TableNameResolver> ClusterAdminClient::BuildTableNameResolver() {
  return TableNameResolver(VERIFY_RESULT(yb_client_->ListTables()),
                           VERIFY_RESULT(yb_client_->ListNamespaces()));
}

string RightPadToUuidWidth(const string &s) {
  return RightPadToWidth(s, kNumCharactersInUuid);
}

Result<TypedNamespaceName> ParseNamespaceName(const std::string& full_namespace_name,
                                              const YQLDatabase default_if_no_prefix) {
  const auto parts = SplitByDot(full_namespace_name);
  return ResolveNamespaceName(parts.prefix, parts.value, default_if_no_prefix);
}

}  // namespace tools
}  // namespace yb
