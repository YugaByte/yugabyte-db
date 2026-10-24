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
// The following only applies to changes made to this file as part of YugabyteDB development.
//
// Portions Copyright (c) YugabyteDB, Inc.
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

#include "yb/client/client-internal.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/stringize.hpp>

#include "yb/client/client_master_rpc.h"
#include "yb/client/meta_cache.h"
#include "yb/client/table_info.h"

#include "yb/common/common_util.h"
#include "yb/common/redis_constants_common.h"
#include "yb/common/schema.h"
#include "yb/common/schema_pbutil.h"
#include "yb/common/tablespace_parser.h"

#include "yb/gutil/bind.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/strings/join.h"

#include "yb/master/master_admin.proxy.h"
#include "yb/master/master_backup.proxy.h"
#include "yb/master/master_client.proxy.h"
#include "yb/master/master_cluster.proxy.h"
#include "yb/master/master_dcl.proxy.h"
#include "yb/master/master_ddl.proxy.h"
#include "yb/master/master_defaults.h"
#include "yb/master/master_encryption.proxy.h"
#include "yb/master/master_replication.proxy.h"
#include "yb/master/master_rpc.h"
#include "yb/master/master_test.proxy.h"
#include "yb/master/master_util.h"

#include "yb/qlexpr/index.h"

#include "yb/rpc/messenger.h"
#include "yb/rpc/rpc.h"
#include "yb/rpc/rpc_controller.h"

#include "yb/server/clock.h"

#include "yb/util/atomic.h"
#include "yb/util/format.h"
#include "yb/util/logging.h"
#include "yb/util/monotime.h"
#include "yb/util/net/net_util.h"
#include "yb/util/result.h"
#include "yb/util/scope_exit.h"
#include "yb/util/status_format.h"
#include "yb/util/status_log.h"
#include "yb/util/string_util.h"

using namespace std::literals;

DEFINE_test_flag(bool, assert_local_tablet_server_selected, false, "Verify that SelectTServer "
    "selected the local tablet server. Also verify that ReplicaSelection is equal "
    "to CLOSEST_REPLICA");

DEFINE_test_flag(string, assert_tablet_server_select_is_in_zone, "",
    "Verify that SelectTServer selected a tablet server in the AZ specified by this flag.");

DEFINE_RUNTIME_uint32(change_metadata_backoff_max_jitter_ms, 0,
    "Max jitter (in ms) in the exponential backoff loop that checks if a change metadata operation "
    "is finished. Only used for colocated table creation for now.");

DEFINE_RUNTIME_uint32(change_metadata_backoff_init_exponent, 1,
    "Initial exponent of 2 in the exponential backoff loop that checks if a change metadata "
    "operation is finished. Only used for colocated table creation for now.");

DECLARE_int64(reset_master_leader_timeout_ms);

DECLARE_string(flagfile);

DECLARE_bool(ysql_yb_enable_ddl_savepoint_support);

namespace yb {

using std::set;
using std::shared_ptr;
using std::string;
using std::vector;
using std::pair;

using namespace std::placeholders;

using master::GetLeaderMasterRpc;
using master::MasterErrorPB;

namespace client {

using internal::GetTablegroupSchemaRpc;
using internal::GetTableSchemaRpc;
using internal::RemoteTablet;
using internal::RemoteTabletServer;
using internal::UpdateLocalTsState;

template<class ProxyClass, class RespClass>
class SyncClientMasterRpc : public internal::ClientMasterRpcBase {
 public:
  using SyncLeaderMasterFunc = std::function<void(
    ProxyClass*, rpc::RpcController*, const rpc::ResponseCallback& callback)>;

  template <class ClientData>
  explicit SyncClientMasterRpc(
      ClientData* client, CoarseTimePoint deadline, RespClass* resp,
      const std::string& name, const SyncLeaderMasterFunc& func)
      : ClientMasterRpcBase(client, deadline),
        resp_(resp), name_(name), func_(func) {
    this->mutable_retrier()->mutable_controller()->set_invoke_callback_mode(
      rpc::InvokeCallbackMode::kReactorThread);
  }

  void CallRemoteMethod() override {
    auto master_proxy = this->template master_proxy<ProxyClass>();
    func_(master_proxy.get(), this->mutable_retrier()->mutable_controller(),
          std::bind(&SyncClientMasterRpc::Finished, this, Status::OK()));
  }

  void ProcessResponse(const Status& status) override {
    synchronizer_.StatusCB(status);
  }

  std::string ToString() const override {
    return name_;
  }

  Synchronizer& synchronizer() {
    return synchronizer_;
  }

  Status ResponseStatus() override {
    return internal::StatusFromResp(*resp_);
  }

  bool ShouldRetry(const Status& status) override;

 private:
  RespClass* resp_;
  std::string name_;
  SyncLeaderMasterFunc func_;
  Synchronizer synchronizer_;
};

template <class ProxyClass, class ReqClass, class RespClass>
Status YBClient::Data::SyncLeaderMasterRpc(
    CoarseTimePoint deadline, const ReqClass& req, RespClass* resp, const char* func_name,
    const SyncLeaderMasterFunc<ProxyClass, ReqClass, RespClass>& func, int* attempts) {
  running_sync_requests_.fetch_add(1, std::memory_order_acquire);
  auto se = ScopeExit([this] {
    running_sync_requests_.fetch_sub(1, std::memory_order_acquire);
  });

  RSTATUS_DCHECK(deadline != CoarseTimePoint(), InvalidArgument, "Deadline is not set");

  auto rpc = std::make_shared<SyncClientMasterRpc<ProxyClass, RespClass>>(
      this, deadline, resp, func_name,
      [func, &req, resp](ProxyClass* proxy, rpc::RpcController* controller,
                         const rpc::ResponseCallback& callback) {
        (proxy->*func)(req, resp, controller, callback);
      });
  SCOPED_WAIT_STATUS(YBClient_WaitingOnMaster);
  RETURN_NOT_OK(rpcs_.RegisterAndStartStatus(rpc, rpc->RpcHandle()));
  auto result = rpc->synchronizer().Wait();
  if (attempts) {
    *attempts = rpc->num_attempts();
  }
  return result;
}

#define YB_CLIENT_SPECIALIZE(RequestTypePB, ResponseTypePB, Service) \
    using master::RequestTypePB; \
    using master::ResponseTypePB; \
    template Status YBClient::Data::SyncLeaderMasterRpc( \
        CoarseTimePoint deadline, const RequestTypePB& req, ResponseTypePB* resp, \
        const char* func_name,                                       \
        const SyncLeaderMasterFunc<                                  \
            master::BOOST_PP_CAT(BOOST_PP_CAT(Master, Service), Proxy),            \
            RequestTypePB, ResponseTypePB>& func, \
        int* attempts);

#define YB_CLIENT_SPECIALIZE_SIMPLE_EX(service, prefix) \
    YB_CLIENT_SPECIALIZE(BOOST_PP_CAT(prefix, RequestPB), BOOST_PP_CAT(prefix, ResponsePB), service)

#define YB_CLIENT_SPECIALIZE_SIMPLE(prefix) \
    YB_CLIENT_SPECIALIZE_SIMPLE_EX(Ddl, prefix)

// Explicit specialization for callers outside this compilation unit.
// These are not actually exposed outside, but it's nice to auto-add using directive.
YB_CLIENT_SPECIALIZE_SIMPLE(AlterNamespace);
YB_CLIENT_SPECIALIZE_SIMPLE(AlterTable);
YB_CLIENT_SPECIALIZE_SIMPLE(BackfillIndex);
YB_CLIENT_SPECIALIZE_SIMPLE(ChangeMasterClusterConfig);
YB_CLIENT_SPECIALIZE_SIMPLE(CheckIfPitrActive);
YB_CLIENT_SPECIALIZE_SIMPLE(CreateNamespace);
YB_CLIENT_SPECIALIZE_SIMPLE(CreateTable);
YB_CLIENT_SPECIALIZE_SIMPLE(CreateTablegroup);
YB_CLIENT_SPECIALIZE_SIMPLE(CreateUDType);
YB_CLIENT_SPECIALIZE_SIMPLE(DeleteNamespace);
YB_CLIENT_SPECIALIZE_SIMPLE(DeleteTable);
YB_CLIENT_SPECIALIZE_SIMPLE(DeleteTablegroup);
YB_CLIENT_SPECIALIZE_SIMPLE(DeleteUDType);
YB_CLIENT_SPECIALIZE_SIMPLE(FlushTables);
YB_CLIENT_SPECIALIZE_SIMPLE(GetBackfillStatus);
YB_CLIENT_SPECIALIZE_SIMPLE(GetTablegroupSchema);
YB_CLIENT_SPECIALIZE_SIMPLE(GetColocatedTabletSchema);
YB_CLIENT_SPECIALIZE_SIMPLE(GetCompactionStatus);
YB_CLIENT_SPECIALIZE_SIMPLE(GetMasterClusterConfig);
YB_CLIENT_SPECIALIZE_SIMPLE(GetNamespaceInfo);
YB_CLIENT_SPECIALIZE_SIMPLE(GetTableSchema);
YB_CLIENT_SPECIALIZE_SIMPLE(GetUDTypeInfo);
YB_CLIENT_SPECIALIZE_SIMPLE(IsAlterTableDone);
YB_CLIENT_SPECIALIZE_SIMPLE(IsCreateNamespaceDone);
YB_CLIENT_SPECIALIZE_SIMPLE(IsCreateTableDone);
YB_CLIENT_SPECIALIZE_SIMPLE(IsDeleteNamespaceDone);
YB_CLIENT_SPECIALIZE_SIMPLE(IsDeleteTableDone);
YB_CLIENT_SPECIALIZE_SIMPLE(IsFlushTablesDone);
YB_CLIENT_SPECIALIZE_SIMPLE(IsTruncateTableDone);
YB_CLIENT_SPECIALIZE_SIMPLE(ListClones);
YB_CLIENT_SPECIALIZE_SIMPLE(ListNamespaces);
YB_CLIENT_SPECIALIZE_SIMPLE(ListTablegroups);
YB_CLIENT_SPECIALIZE_SIMPLE(ListTables);
YB_CLIENT_SPECIALIZE_SIMPLE(ListUDTypes);
YB_CLIENT_SPECIALIZE_SIMPLE(TruncateTable);
YB_CLIENT_SPECIALIZE_SIMPLE(ValidateReplicationInfo);
YB_CLIENT_SPECIALIZE_SIMPLE(GetObjectLockStatus);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Encryption, GetFullUniverseKeyRegistry);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Admin, AddTransactionStatusTablet);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Admin, AreNodesSafeToTakeDown);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Admin, CreateTransactionStatusTable);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Admin, WaitForYsqlBackendsCatalogVersion);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Backup, CloneNamespace);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Backup, CreateSnapshot);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Backup, DeleteSnapshot);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Backup, ListSnapshots);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Client, GetIndexBackfillProgress);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Client, GetTableLocations);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Client, GetTabletLocations);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Client, GetTransactionStatusTablets);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Client, GetYsqlCatalogConfig);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Client, RedisConfigGet);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Client, RedisConfigSet);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Client, ReservePgsqlOids);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Client, GetYsqlYbSystemTableInfo);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Client, GetStatefulServiceLocation);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Client, GetTabletsMetadata);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Cluster, GetAutoFlagsConfig);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Cluster, ValidateAutoFlagsConfig);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Cluster, IsLoadBalanced);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Cluster, IsLoadBalancerIdle);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Cluster, ListLiveTabletServers);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Cluster, ListMasters);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Cluster, ListTabletServers);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Dcl, AlterRole);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Dcl, CreateRole);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Dcl, DeleteRole);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Dcl, GetPermissions);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Dcl, GrantRevokePermission);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Dcl, GrantRevokeRole);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, AlterUniverseReplication);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, GetXClusterSafeTimeForNamespace);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, CreateCDCStream);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, DeleteCDCStream);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, GetCDCDBStreamInfo);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, GetCDCStream);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, ListCDCStreams);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, UpdateCDCStream);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, RemoveTablesFromCDCSDKStream);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, IsObjectPartOfXRepl);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, IsNamespacePartOfCDCSDK);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, IsBootstrapRequired);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, GetUDTypeMetadata);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, GetTableSchemaFromSysCatalog);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, UpdateConsumerOnProducerSplit);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, UpdateConsumerOnProducerMetadata);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, InsertHistoricalColocatedSchemaPacking);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, InsertPackedSchemaForXClusterTarget);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, HandleNewSchemaForAutomaticXClusterTarget);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, GetXClusterSafeTime);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, BootstrapProducer);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, XClusterReportNewAutoFlagConfigVersion);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, XClusterAddNamespaceToOutboundReplicationGroup);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, XClusterRemoveNamespaceFromOutboundReplicationGroup);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, SetupUniverseReplication);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, IsSetupUniverseReplicationDone);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, CreateXClusterReplication);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, IsCreateXClusterReplicationDone);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, XClusterCreateOutboundReplicationGroup);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, XClusterDeleteOutboundReplicationGroup);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, AddNamespaceToXClusterReplication);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, IsAlterXClusterReplicationDone);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, DeleteUniverseReplication);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, RepairOutboundXClusterReplicationGroupAddTable);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, RepairOutboundXClusterReplicationGroupRemoveTable);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, DeleteXClusterWalAnchorStreams);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, GetXClusterOutboundReplicationGroups);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, GetXClusterOutboundReplicationGroupInfo);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, GetUniverseReplications);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, GetUniverseReplicationInfo);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, WaitForReplicationDrain);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, XClusterEnsureSequenceUpdatesAreInWal);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, XClusterStopPersisting);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, XClusterStartPersisting);
YB_CLIENT_SPECIALIZE_SIMPLE_EX(Replication, XClusterGetPersistenceBlockMap);

#define YB_CLIENT_SPECIALIZE_SIMPLE_EX_EACH(i, data, set) YB_CLIENT_SPECIALIZE_SIMPLE_EX set

#define YB_CLIENT_SPECIALIZE_SIMPLE_FOR(rpcs) \
  BOOST_PP_SEQ_FOR_EACH(YB_CLIENT_SPECIALIZE_SIMPLE_EX_EACH, ~, rpcs)

template<class ProxyClass, class RespClass>
bool SyncClientMasterRpc<ProxyClass, RespClass>::ShouldRetry(const Status& status) {
  return internal::ClientMasterRpcBase::ShouldRetry(status);
}

template<>
bool SyncClientMasterRpc<master::MasterAdminProxy,
                         WaitForYsqlBackendsCatalogVersionResponsePB>::ShouldRetry(
    const Status& status) {
  return status.IsTryAgain();
}

// Checks if the Tablet locations are valid - Partition keys are sorted with no overlaps.
Status CheckTabletLocations(
    const google::protobuf::RepeatedPtrField<master::TabletLocationsPB>& locations,
    tserver::AllowSplitTablet allow_split_tablets) {
  const std::string* prev_partition_end = nullptr;
  for (const master::TabletLocationsPB& loc : locations) {
    LOG_IF(DFATAL, !allow_split_tablets && loc.split_tablet_ids().size() > 0)
        << "Processing remote tablet location with split children id set: "
        << loc.ShortDebugString() << " when allow_split_tablets was set to false.";
    if (prev_partition_end && *prev_partition_end > loc.partition().partition_key_start()) {
      LOG(DFATAL) << "There should be no overlaps in tablet partitions and they should be sorted "
                  << "by partition_key_start. Prev partition end: "
                  << Slice(*prev_partition_end).ToDebugHexString() << ", current partition start: "
                  << Slice(loc.partition().partition_key_start()).ToDebugHexString()
                  << ". Tablet locations: " << [&locations] {
                       std::string result;
                       for (auto& loc : locations) {
                         result += "\n  " + AsString(loc);
                       }
                       return result;
                     }();
      return STATUS(IllegalState, "Wrong order or overlaps in partitions");
    }
    prev_partition_end = &loc.partition().partition_key_end();
  }
  return Status::OK();
}

YBClient::Data::Data()
    : leader_master_rpc_(rpcs_.InvalidHandle()),
      latest_observed_hybrid_time_(YBClient::kNoHybridTime),
      request_id_allocator_(internal::CreateRequestIdAllocator()),
      log_prefix_(Format("Client $0: ", request_id_allocator_->client_id())) {
  for(auto& cache : tserver_count_cached_) {
    cache.store(0, std::memory_order_relaxed);
  }
}

YBClient::Data::~Data() {
  rpcs_.Shutdown();
}

RemoteTabletServer* YBClient::Data::SelectTServer(RemoteTablet* rt,
                                                  const ReplicaSelection selection,
                                                  const set<string>& blacklist,
                                                  vector<RemoteTabletServer*>* candidates) {
  RemoteTabletServer* ret = nullptr;
  candidates->clear();
  if (PREDICT_FALSE(FLAGS_TEST_assert_local_tablet_server_selected ||
                    !FLAGS_TEST_assert_tablet_server_select_is_in_zone.empty()) &&
      selection != CLOSEST_REPLICA) {
    LOG(FATAL) << "Invalid ReplicaSelection " << selection;
  }

  switch (selection) {
    case LEADER_ONLY: {
      ret = rt->LeaderTServer();
      if (ret != nullptr) {
        candidates->push_back(ret);
        if (ContainsKey(blacklist, ret->permanent_uuid())) {
          ret = nullptr;
        }
      }
      break;
    }
    case CLOSEST_REPLICA:
    case FIRST_REPLICA: {
      if (PREDICT_TRUE(FLAGS_TEST_assert_tablet_server_select_is_in_zone.empty())) {
        rt->GetRemoteTabletServers(candidates);
      } else {
        rt->GetRemoteTabletServers(candidates, internal::IncludeFailedReplicas::kTrue);
      }

      // Filter out all the blacklisted candidates.
      vector<RemoteTabletServer*> filtered;
      for (RemoteTabletServer* rts : *candidates) {
        if (!ContainsKey(blacklist, rts->permanent_uuid())) {
          filtered.push_back(rts);
        } else {
          VLOG(1) << "Excluding blacklisted tserver " << rts->permanent_uuid();
        }
      }
      if (selection == FIRST_REPLICA) {
        if (!filtered.empty()) {
          ret = filtered[0];
        }
      } else if (selection == CLOSEST_REPLICA) {
        // Choose the closest replica.
        LocalityLevel best_locality_level = LocalityLevel::kNone;
        for (RemoteTabletServer* rts : filtered) {
          if (IsTabletServerLocal(*rts)) {
            ret = rts;
            // If the tserver is local, we are done here.
            break;
          } else {
            auto locality_level = rts->LocalityLevelWith(cloud_info_pb_);
            if (locality_level > best_locality_level) {
              ret = rts;
              best_locality_level = locality_level;
            }
          }
        }

        // If ret is not null here, it should point to the closest replica from the client.

        // Fallback to a random replica if none are local.
        if (ret == nullptr && !filtered.empty()) {
          ret = filtered[rand() % filtered.size()];
        }
      }
      break;
    }
    default:
      FATAL_INVALID_ENUM_VALUE(ReplicaSelection, selection);
  }
  if (PREDICT_FALSE(FLAGS_TEST_assert_local_tablet_server_selected) && !IsTabletServerLocal(*ret)) {
    LOG(FATAL) << "Selected replica is not the local tablet server";
  }
  if (PREDICT_FALSE(!FLAGS_TEST_assert_tablet_server_select_is_in_zone.empty())) {
    if (ret->TEST_PlacementZone() != FLAGS_TEST_assert_tablet_server_select_is_in_zone) {
      string msg = Format("\nZone placement:\nNumber of candidates: $0\n", candidates->size());
      for (RemoteTabletServer* rts : *candidates) {
        msg += Format("Replica: $0 in zone $1\n", rts->ToString(), rts->TEST_PlacementZone());
      }
      LOG(FATAL) << "Selected replica " << ret->ToString()
                 << " is in zone " << ret->TEST_PlacementZone()
                 << " instead of the expected zone "
                 << FLAGS_TEST_assert_tablet_server_select_is_in_zone
                 << " Cloud info: " << cloud_info_pb_.ShortDebugString()
                 << " for selection policy " << selection
                 << msg;
    }
  }

  return ret;
}

Status YBClient::Data::GetTabletServer(YBClient* client,
                                       const scoped_refptr<RemoteTablet>& rt,
                                       ReplicaSelection selection,
                                       const set<string>& blacklist,
                                       vector<RemoteTabletServer*>* candidates,
                                       RemoteTabletServer** ts) {
  // TODO: write a proper async version of this for async client.
  RemoteTabletServer* ret = SelectTServer(rt.get(), selection, blacklist, candidates);
  if (PREDICT_FALSE(ret == nullptr)) {
    // Construct a blacklist string if applicable.
    string blacklist_string = "";
    if (!blacklist.empty()) {
      blacklist_string = Format("(blacklist replicas $0)", JoinStrings(blacklist, ", "));
    }
    return STATUS_FORMAT(
        ServiceUnavailable,
        "No $0 for tablet $1 $2",
        selection == LEADER_ONLY ? "LEADER" : "replicas",
        rt->tablet_id(),
        blacklist_string);
  }
  RETURN_NOT_OK(ret->InitProxy(client));

  *ts = ret;
  return Status::OK();
}

Status YBClient::Data::CreateTable(YBClient* client,
                                   const CreateTableRequestPB& req,
                                   const YBSchema& schema,
                                   CoarseTimePoint deadline,
                                   string* table_id) {
  CreateTableResponsePB resp;

  int attempts = 0;
  Status s = SyncLeaderMasterRpc(
      deadline, req, &resp, "CreateTable", &master::MasterDdlProxy::CreateTableAsync,
      &attempts);
  // Set the table id even if there was an error. This is useful when the error is IsAlreadyPresent
  // so that we can wait for the existing table to be available to receive requests.
  *table_id = resp.table_id();

  // Handle special cases based on resp.error().
  if (resp.has_error()) {
    LOG_IF(DFATAL, s.ok()) << "Expecting error status if response has error: " <<
        resp.error().code() << " Status: " << resp.error().status().ShortDebugString();

    if (resp.error().code() == MasterErrorPB::OBJECT_ALREADY_PRESENT && attempts > 1) {
      // If the table already exists and the number of attempts is >
      // 1, then it means we may have succeeded in creating the
      // table, but client didn't receive the successful
      // response (e.g., due to failure before the successful
      // response could be sent back, or due to a I/O pause or a
      // network blip leading to a timeout, etc...)
      YBTableInfo info;
      const string keyspace = req.has_namespace_()
          ? req.namespace_().name()
          : (req.name() == common::kRedisTableName ? common::kRedisKeyspaceName : "");
      const YQLDatabase db_type = req.has_namespace_() && req.namespace_().has_database_type()
          ? req.namespace_().database_type()
          : (keyspace.empty() ? YQL_DATABASE_CQL : master::GetDefaultDatabaseType(keyspace));

      // Identify the table by name.
      LOG_IF(DFATAL, keyspace.empty()) << "No keyspace. Request:\n" << req.DebugString();
      const YBTableName table_name(db_type, keyspace, req.name());

      // A fix for https://yugabyte.atlassian.net/browse/ENG-529:
      // If we've been retrying table creation, and the table is now in the process is being
      // created, we can sometimes see an empty schema. Wait until the table is fully created
      // before we compare the schema.
      RETURN_NOT_OK_PREPEND(
          WaitForCreateTableToFinish(client, table_name, resp.table_id(), deadline),
          Format("Failed waiting for table $0 to finish being created", table_name.ToString()));

      RETURN_NOT_OK_PREPEND(
          GetTableSchema(client, table_name, deadline, &info),
          Format("Unable to check the schema of table $0", table_name.ToString()));
      if (!schema.Equals(info.schema)) {
        string msg = Format("Table $0 already exists with a different "
                            "schema. Requested schema was: $1, actual schema is: $2",
                            table_name,
                            internal::GetSchema(schema),
                            internal::GetSchema(info.schema));
        LOG(WARNING) << msg;
        return STATUS(AlreadyPresent, msg);
      }

      // The partition schema in the request can be empty.
      // If there are user partition schema in the request - compare it with the received one.
      if (req.partition_schema().hash_bucket_schemas_size() > 0) {
        dockv::PartitionSchema partition_schema;
        // We need to use the schema received from the server, because the user-constructed
        // schema might not have column ids.
        RETURN_NOT_OK(dockv::PartitionSchema::FromPB(
            req.partition_schema(), internal::GetSchema(info.schema), &partition_schema));
        if (!partition_schema.Equals(info.partition_schema)) {
          string msg = Format(
              "Table $0 already exists with a different partition schema. "
              "Requested partition schema was: $1, actual partition schema is: $2",
              table_name.ToString(),
              partition_schema.DebugString(internal::GetSchema(schema)),
              info.partition_schema.DebugString(internal::GetSchema(info.schema)));
          LOG(WARNING) << msg;
          return STATUS(AlreadyPresent, msg);
        }
      }

      return Status::OK();
    }

    return StatusFromPB(resp.error().status());
  }

  // Use the status only if the response has no error.
  return s;
}

Status YBClient::Data::IsCreateTableInProgress(
    YBClient* client,
    const YBTableName& table_name,
    const TableId& table_id,
    CoarseTimePoint deadline,
    bool* create_in_progress) {
  DCHECK_ONLY_NOTNULL(create_in_progress);
  IsCreateTableDoneRequestPB req;
  IsCreateTableDoneResponsePB resp;
  if (table_name.has_table()) {
    table_name.SetIntoTableIdentifierPB(req.mutable_table());
  }
  if (!table_id.empty()) {
    req.mutable_table()->set_table_id(table_id);
  }
  if (!req.has_table()) {
    *create_in_progress = false;
    return STATUS(InternalError, "Cannot query IsCreateTableInProgress without table info");
  }

  // This check is fast, so use the default RPC timeout.  The deadline is for the whole create,
  // which can be long (e.g., xcluster_automatic_target_create_table_ddl_rpc_timeout_sec).
  const auto rpc_deadline = std::min(deadline, CoarseMonoClock::Now() + default_rpc_timeout_);
  auto status = SyncLeaderMasterRpc(
      rpc_deadline, req, &resp, "IsCreateTableDone",
      &master::MasterDdlProxy::IsCreateTableDoneAsync);

  *create_in_progress = !resp.done();
  return status;
}

Status YBClient::Data::WaitForCreateTableToFinish(
    YBClient* client,
    const YBTableName& table_name,
    const TableId& table_id,
    CoarseTimePoint deadline,
    const uint32_t max_jitter_ms,
    const uint32_t init_exponent) {
  return RetryUntilShutdown(
      deadline, "Waiting on Create Table to be completed", "Timed out waiting for Table Creation",
      std::bind(
          &YBClient::Data::IsCreateTableInProgress, this, client, table_name, table_id, _1, _2),
      2s, max_jitter_ms, init_exponent);
}

Status YBClient::Data::DeleteTable(
    YBClient* client,
    const YBTableName& table_name,
    const TableId& table_id,
    const bool is_index_table,
    CoarseTimePoint deadline,
    YBTableName* indexed_table_name,
    bool wait,
    const TransactionMetadata *txn,
    SubTransactionId sub_transaction_id) {
  DeleteTableRequestPB req;
  DeleteTableResponsePB resp;
  int attempts = 0;

  if (table_name.has_table()) {
    table_name.SetIntoTableIdentifierPB(req.mutable_table());
  }
  if (!table_id.empty()) {
    req.mutable_table()->set_table_id(table_id);
  }
  if (YsqlDdlRollbackEnabled() && txn) {
    // If 'txn' is set, this means this delete operation should actually result in the
    // deletion of table data only if this transaction is a success. Therefore ensure that
    // 'wait' is not set, because it makes no sense to wait for the deletion to complete if we want
    // to postpone the deletion until end of transaction.
    DCHECK(!wait);
    txn->ToPB(req.mutable_transaction());
    req.set_ysql_yb_ddl_rollback_enabled(true);
    if (YsqlDdlSavepointEnabled()) {
      req.set_sub_transaction_id(sub_transaction_id);
    }
  }
  req.set_is_index_table(is_index_table);
  const Status status = SyncLeaderMasterRpc(
      deadline, req, &resp, "DeleteTable", &master::MasterDdlProxy::DeleteTableAsync,
      &attempts);

  // Handle special cases based on resp.error().
  if (resp.has_error()) {
    LOG_IF(DFATAL, status.ok())
        << "Expecting error status if response has error: " << resp.error().code()
        << " Status: " << resp.error().status().ShortDebugString();

    if (resp.error().code() == MasterErrorPB::OBJECT_NOT_FOUND && attempts > 1) {
      // A prior attempt to delete the table has succeeded, but
      // appeared as a failure to the client due to, e.g., an I/O or
      // network issue.
      // Good case - go through - to 'return Status::OK()'
    } else {
      return status;
    }
  } else {
    // Check the status only if the response has no error.
    RETURN_NOT_OK(status);
  }

  // Spin until the table is fully deleted, if requested.
  VLOG(3) << "Got response " << yb::ToString(resp);
  if (wait) {
    // Wait for the deleted tables to be gone.
    if (resp.deleted_table_ids_size() > 0) {
      for (const auto& table_id : resp.deleted_table_ids()) {
        RETURN_NOT_OK(WaitForDeleteTableToFinish(client, table_id, deadline));
        VLOG(2) << "Waited for table to be deleted " << table_id;
      }
    } else if (resp.has_table_id()) {
      // for backwards compatibility, in case the master is not yet using deleted_table_ids.
      RETURN_NOT_OK(WaitForDeleteTableToFinish(client, resp.table_id(), deadline));
      VLOG(2) << "Waited for table to be deleted " << resp.table_id();
    }

    // In case this table is an index, wait for the indexed table to remove reference to index
    // table.
    if (resp.has_indexed_table()) {
      auto res = WaitUntilIndexPermissionsAtLeast(
          client,
          resp.indexed_table().table_id(),
          resp.table_id(),
          IndexPermissions::INDEX_PERM_NOT_USED,
          deadline);
      if (!res && !res.status().IsNotFound()) {
        LOG(WARNING) << "Waiting for the index to be deleted from the indexed table, got " << res;
        return res.status();
      }
    }
  }

  // Return indexed table name if requested.
  if (resp.has_indexed_table() && indexed_table_name != nullptr) {
    indexed_table_name->GetFromTableIdentifierPB(resp.indexed_table());
  }

  if (req.ysql_yb_ddl_rollback_enabled()) {
    LOG(INFO) << "Marked table " << (!table_id.empty() ? table_id : table_name.ToString())
              << " for deletion at the end of transaction " << txn->ToString();
  } else {
    LOG(INFO) << "Deleted table " << (!table_id.empty() ? table_id : table_name.ToString());
  }
  return Status::OK();
}

Status YBClient::Data::IsDeleteTableInProgress(
    YBClient* client,
    const TableId& table_id,
    CoarseTimePoint deadline,
    bool* delete_in_progress) {
  DCHECK_ONLY_NOTNULL(delete_in_progress);
  IsDeleteTableDoneRequestPB req;
  IsDeleteTableDoneResponsePB resp;
  req.set_table_id(table_id);

  const auto status = SyncLeaderMasterRpc(
      deadline, req, &resp, "IsDeleteTableDone", &master::MasterDdlProxy::IsDeleteTableDoneAsync);
  if (resp.has_error()) {
    // Set 'retry' variable in 'RetryFunc()' function into FALSE to stop the retry loop.
    // 'RetryFunc()' is called from 'WaitForDeleteTableToFinish()'.
    *delete_in_progress = false; // Do not retry on error.
  }

  RETURN_NOT_OK(status);
  *delete_in_progress = !resp.done();
  return Status::OK();
}

Status YBClient::Data::WaitForDeleteTableToFinish(
    YBClient* client,
    const TableId& table_id,
    CoarseTimePoint deadline) {
  return RetryUntilShutdown(
      deadline, "Waiting on Delete Table to be completed", "Timed out waiting for Table Deletion",
      std::bind(&YBClient::Data::IsDeleteTableInProgress, this, client, table_id, _1, _2));
}

Status YBClient::Data::TruncateTables(
    YBClient* client,
    const TableIds& table_ids,
    CoarseTimePoint deadline,
    bool wait) {
  TruncateTableRequestPB req;
  TruncateTableResponsePB resp;

  for (const auto& table_id : table_ids) {
    req.add_table_ids(table_id);
  }
  RETURN_NOT_OK(SyncLeaderMasterRpc(
      deadline, req, &resp, "TruncateTable", &master::MasterDdlProxy::TruncateTableAsync));

  // Spin until the table is fully truncated, if requested.
  if (wait) {
    for (const auto& table_id : table_ids) {
      // For YCQL tables this call also must wait for the related indexes.
      // See CatalogManager::IsTruncateTableDone for details.
      RETURN_NOT_OK(WaitForTruncateTableToFinish(client, table_id, deadline));
    }
  }

  LOG(INFO) << "Truncated table(s) " << JoinStrings(table_ids, ",");
  return Status::OK();
}

Status YBClient::Data::IsTruncateTableInProgress(
    YBClient* client,
    const TableId& table_id,
    CoarseTimePoint deadline,
    bool* truncate_in_progress) {
  DCHECK_ONLY_NOTNULL(truncate_in_progress);
  IsTruncateTableDoneRequestPB req;
  IsTruncateTableDoneResponsePB resp;
  req.set_table_id(table_id);
  RETURN_NOT_OK(SyncLeaderMasterRpc(
      deadline, req, &resp, "IsTruncateTableDone",
      &master::MasterDdlProxy::IsTruncateTableDoneAsync));

  *truncate_in_progress = !resp.done();
  return Status::OK();
}

Status YBClient::Data::WaitForTruncateTableToFinish(
    YBClient* client,
    const TableId& table_id,
    CoarseTimePoint deadline) {
  return RetryUntilShutdown(
      deadline, "Waiting on Truncate Table to be completed",
      "Timed out waiting for Table Truncation",
      std::bind(&YBClient::Data::IsTruncateTableInProgress, this, client, table_id, _1, _2));
}

Status YBClient::Data::AlterNamespace(YBClient* client,
                                      const AlterNamespaceRequestPB& req,
                                      CoarseTimePoint deadline) {
  AlterNamespaceResponsePB resp;
  return SyncLeaderMasterRpc(
      deadline, req, &resp, "AlterNamespace", &master::MasterDdlProxy::AlterNamespaceAsync);
}

Status YBClient::Data::CreateTablegroup(
    YBClient* client,
    CoarseTimePoint deadline,
    const NamespaceName& namespace_name,
    const NamespaceId& namespace_id,
    const TablegroupId& tablegroup_id,
    const TablespaceId& tablespace_id,
    const TransactionMetadata* txn,
    const SubTransactionId sub_transaction_id) {
  CreateTablegroupRequestPB req;
  CreateTablegroupResponsePB resp;
  req.set_id(tablegroup_id);
  req.set_namespace_id(namespace_id);
  req.set_namespace_name(namespace_name);

  if (!tablespace_id.empty()) {
    req.set_tablespace_id(tablespace_id);
  }

  if (txn) {
    txn->ToPB(req.mutable_transaction());
    req.set_ysql_yb_ddl_rollback_enabled(YsqlDdlRollbackEnabled());
    // sub_transaction_id can be 0 when the CREATE INDEX statement is executed in separate DDL
    // transactions even when transactional DDL is enabled. For separate DDL transactions, we do not
    // support savepoints, so don't forward an invalid value of sub_transaction_id to yb-master.
    if (YsqlDdlSavepointEnabled() && sub_transaction_id >= kMinSubTransactionId) {
      req.set_sub_transaction_id(sub_transaction_id);
    }
  }

  int attempts = 0;
  RETURN_NOT_OK(SyncLeaderMasterRpc(
      deadline, req, &resp, "CreateTablegroup",
      &master::MasterDdlProxy::CreateTablegroupAsync, &attempts));

  // This case should not happen but need to validate contents since fields are optional in PB.
  SCHECK(resp.has_parent_table_id() && resp.has_parent_table_name(),
         InternalError,
         "Parent table information not found in CREATE TABLEGROUP response");

  const YBTableName table_name(YQL_DATABASE_PGSQL, namespace_name, resp.parent_table_name());

  // Handle special cases based on resp.error().
  if (resp.has_error()) {
    if (resp.error().code() == master::MasterErrorPB::OBJECT_ALREADY_PRESENT && attempts > 1) {
      // If the table already exists and the number of attempts is >
      // 1, then it means we may have succeeded in creating the
      // table, but client didn't receive the successful
      // response (e.g., due to failure before the successful
      // response could be sent back, or due to a I/O pause or a
      // network blip leading to a timeout, etc...)
      YBTableInfo info;

      // A fix for https://yugabyte.atlassian.net/browse/ENG-529:
      // If we've been retrying table creation, and the table is now in the process is being
      // created, we can sometimes see an empty schema. Wait until the table is fully created
      // before we compare the schema.
      RETURN_NOT_OK_PREPEND(
          WaitForCreateTableToFinish(client, table_name, resp.parent_table_id(), deadline),
          Format(
              "Failed waiting for a parent table $0 to finish being created",
              table_name.ToString()));

      RETURN_NOT_OK_PREPEND(
          GetTableSchema(client, table_name, deadline, &info),
          Format("Unable to check the schema of parent table $0", table_name.ToString()));

      YBSchemaBuilder schema_builder;
      schema_builder.AddColumn("parent_column")->Type(DataType::BINARY)->PrimaryKey();
      YBSchema ybschema;
      CHECK_OK(schema_builder.Build(&ybschema));

      if (!ybschema.Equals(info.schema)) {
        string msg = Format("Table $0 already exists with a different "
                            "schema. Requested schema was: $1, actual schema is: $2",
                            table_name,
                            internal::GetSchema(ybschema),
                            internal::GetSchema(info.schema));
        LOG(WARNING) << msg;
        return STATUS(AlreadyPresent, msg);
      }

      return Status::OK();
    }

    return StatusFromPB(resp.error().status());
  }

  RETURN_NOT_OK(WaitForCreateTableToFinish(client, table_name, resp.parent_table_id(), deadline));

  return Status::OK();
}

Status YBClient::Data::DeleteTablegroup(
    YBClient* client,
    CoarseTimePoint deadline,
    const TablegroupId& tablegroup_id,
    const TransactionMetadata* txn,
    const SubTransactionId sub_transaction_id) {
  DeleteTablegroupRequestPB req;
  DeleteTablegroupResponsePB resp;
  req.set_id(tablegroup_id);

  // If YSQL DDL Rollback is enabled, the YB-Master will merely mark the tablegroup for deletion
  // and perform the actual deletion only after the transaction commits. Thus there is no point
  // waiting for the table to be deleted here if DDL Rollback is enabled.
  bool wait = true;
  if (txn && YsqlDdlRollbackEnabled()) {
    txn->ToPB(req.mutable_transaction());
    req.set_ysql_yb_ddl_rollback_enabled(true);
    wait = false;
    if (YsqlDdlSavepointEnabled()) {
      req.set_sub_transaction_id(sub_transaction_id);
    }
  }

  int attempts = 0;
  RETURN_NOT_OK(SyncLeaderMasterRpc(
      deadline, req, &resp, "DeleteTablegroup",
      &master::MasterDdlProxy::DeleteTablegroupAsync, &attempts));

  // This case should not happen but need to validate contents since fields are optional in PB.
  SCHECK(resp.has_parent_table_id(),
         InternalError,
         "Parent table information not found in DELETE TABLEGROUP response");

  // Handle special cases based on resp.error().
  if (resp.has_error()) {
    if (resp.error().code() == master::MasterErrorPB::OBJECT_NOT_FOUND && attempts > 1) {
      // A prior attempt to delete the table has succeeded, but
      // appeared as a failure to the client due to, e.g., an I/O or
      // network issue.
      LOG(INFO) << "Tablegroup " << tablegroup_id << " is already deleted.";
      return Status::OK();
    }

    return StatusFromPB(resp.error().status());
  }

  if (wait) {
    RETURN_NOT_OK(WaitForDeleteTableToFinish(client, resp.parent_table_id(), deadline));
  }

  LOG(INFO) << "Deleted tablegroup " << tablegroup_id;
  return Status::OK();
}

Status YBClient::Data::BackfillIndex(YBClient* client,
                                     const YBTableName& index_name,
                                     const TableId& index_id,
                                     CoarseTimePoint deadline,
                                     std::optional<TransactionMetadata> requester_transaction,
                                     bool wait) {
  BackfillIndexRequestPB req;
  BackfillIndexResponsePB resp;

  if (index_name.has_table()) {
    index_name.SetIntoTableIdentifierPB(req.mutable_index_identifier());
  }
  if (!index_id.empty()) {
    req.mutable_index_identifier()->set_table_id(index_id);
  }
  if (requester_transaction.has_value()) {
    requester_transaction->ToPB(req.mutable_requester_transaction());
  }

  RETURN_NOT_OK((SyncLeaderMasterRpc(
      deadline, req, &resp, "BackfillIndex", &master::MasterDdlProxy::BackfillIndexAsync)));

  // Spin until the table is fully backfilled, if requested.
  if (wait) {
    RETURN_NOT_OK(WaitForBackfillIndexToFinish(
        client,
        resp.table_identifier().table_id(),
        index_id,
        deadline));
  }

  LOG(INFO) << "Backfilled index " << req.index_identifier().ShortDebugString();
  return Status::OK();
}

Status YBClient::Data::IsBackfillIndexInProgress(YBClient* client,
                                                 const TableId& table_id,
                                                 const TableId& index_id,
                                                 CoarseTimePoint deadline,
                                                 bool* backfill_in_progress) {
  DCHECK_ONLY_NOTNULL(backfill_in_progress);
  YBTableInfo yb_table_info;
  RETURN_NOT_OK(GetTableSchema(client,
                               table_id,
                               deadline,
                               &yb_table_info));
  const auto* index_info = VERIFY_RESULT(yb_table_info.index_map.FindIndex(index_id));

  *backfill_in_progress = true;
  if (!index_info->backfill_error_message().empty()) {
    *backfill_in_progress = false;
    // backfill_status preserves the error codes (such as the PostgreSQL error code) but may be
    // stale after a rollback: an old master clears only backfill_error_message on success.
    return index_info->backfill_status().ok()
        ? STATUS(Aborted, index_info->backfill_error_message())
        : index_info->backfill_status().CloneAndReplaceCode(Status::kAborted);
  } else if (index_info->index_permissions() > IndexPermissions::INDEX_PERM_DO_BACKFILL) {
    *backfill_in_progress = false;
  }

  return Status::OK();
}

Status YBClient::Data::WaitForBackfillIndexToFinish(
    YBClient* client,
    const TableId& table_id,
    const TableId& index_id,
    CoarseTimePoint deadline) {
  return RetryUntilShutdown(
      deadline,
      "Waiting on Backfill Index to be completed",
      "Timed out waiting for Backfill Index",
      std::bind(
          &YBClient::Data::IsBackfillIndexInProgress, this, client, table_id, index_id, _1, _2));
}

Result<bool> YBClient::Data::IsBackfillIndexStarted(
    YBClient* client, const TableId& index_table_id, const TableId& indexed_table_id,
    CoarseTimePoint deadline) {
  YBTableInfo yb_table_info;
  master::GetTableSchemaResponsePB resp;
  RETURN_NOT_OK(GetTableSchema(
      client, indexed_table_id, deadline, &yb_table_info, master::IncludeHidden::kFalse, &resp));

  for (const auto& index : resp.indexes()) {
    if (index.table_id() == index_table_id) {
      SCHECK_FORMAT(
          index.backfill_error_message().empty(), IllegalState, "Index $0 has backfill error: $1",
          index_table_id, index.backfill_error_message());

      auto index_permissions = index.index_permissions();

      // Anything above INDEX_PERM_READ_WRITE_AND_DELETE means index is being deleted.
      SCHECK_LE(
          index_permissions, INDEX_PERM_READ_WRITE_AND_DELETE, IllegalState,
          Format(
              "Index permissions $0 for index $1 is unexpected",
              IndexPermissions_Name(index_permissions), index_table_id));

      // YSQL indexes start with permission INDEX_PERM_WRITE_AND_DELETE, and directly moves to
      // INDEX_PERM_READ_WRITE_AND_DELETE. The actual stages are tracked in postgres fields
      // indislive, indisready and indisvalid. So use index_permissions to determine if the index is
      // currently backfilling.
      return resp.is_backfilling() ||
             index_permissions == IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE;
    }
  }

  // We can get here if the indexed table schema is not yet fully applied.
  return false;
}

Result<master::GetBackfillStatusResponsePB> YBClient::Data::GetBackfillStatus(
    const std::vector<std::string_view>& table_ids, const CoarseTimePoint deadline) {
  if (table_ids.empty()) {
    return STATUS(InvalidArgument, "At least one table must be specified");
  }

  GetBackfillStatusRequestPB req;
  GetBackfillStatusResponsePB resp;
  req.mutable_index_tables()->Reserve(yb::narrow_cast<int>(table_ids.size()));
  for (auto it = table_ids.begin(); it != table_ids.end(); ++it) {
    req.add_index_tables()->set_table_id(std::string{*it});
  }

  RETURN_NOT_OK(SyncLeaderMasterRpc(
      deadline, req, &resp, "GetBackfillStatus",
      &master::MasterDdlProxy::GetBackfillStatusAsync));

  return resp;
}

Status YBClient::Data::IsCreateNamespaceInProgress(
    YBClient* client, const NamespaceName& namespace_name,
    const std::optional<YQLDatabase>& database_type, const NamespaceId& namespace_id,
    CoarseTimePoint deadline, bool* create_in_progress) {
  DCHECK_ONLY_NOTNULL(create_in_progress);
  IsCreateNamespaceDoneRequestPB req;
  IsCreateNamespaceDoneResponsePB resp;

  req.mutable_namespace_()->set_name(namespace_name);
  if (database_type) {
    req.mutable_namespace_()->set_database_type(*database_type);
  }
  if (!namespace_id.empty()) {
    req.mutable_namespace_()->set_id(namespace_id);
  }

  // RETURN_NOT_OK macro can't take templated function call as param,
  // and SyncLeaderMasterRpc must be explicitly instantiated, else the
  // compiler complains.
  const Status s = SyncLeaderMasterRpc(
      deadline, req, &resp, "IsCreateNamespaceDone",
      &master::MasterDdlProxy::IsCreateNamespaceDoneAsync);

  // IsCreate could return a terminal/done state as FAILED. This would result in an error'd Status.
  if (resp.has_done()) {
    *create_in_progress = !resp.done();
  }

  RETURN_NOT_OK(s);
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  return Status::OK();
}

Status YBClient::Data::WaitForCreateNamespaceToFinish(
    YBClient* client, const NamespaceName& namespace_name,
    const std::optional<YQLDatabase>& database_type, const NamespaceId& namespace_id,
    CoarseTimePoint deadline) {
  return RetryUntilShutdown(
      deadline,
      "Waiting on Create Namespace to be completed",
      "Timed out waiting for Namespace Creation",
      std::bind(
          &YBClient::Data::IsCreateNamespaceInProgress, this, client, namespace_name, database_type,
          namespace_id, _1, _2));
}

Status YBClient::Data::IsDeleteNamespaceInProgress(
    YBClient* client, const NamespaceName& namespace_name,
    const std::optional<YQLDatabase>& database_type, const NamespaceId& namespace_id,
    CoarseTimePoint deadline, bool* delete_in_progress) {
  DCHECK_ONLY_NOTNULL(delete_in_progress);
  IsDeleteNamespaceDoneRequestPB req;
  IsDeleteNamespaceDoneResponsePB resp;

  req.mutable_namespace_()->set_name(namespace_name);
  if (database_type) {
    req.mutable_namespace_()->set_database_type(*database_type);
  }
  if (!namespace_id.empty()) {
    req.mutable_namespace_()->set_id(namespace_id);
  }

  const Status s = SyncLeaderMasterRpc(
      deadline, req, &resp, "IsDeleteNamespaceDone",
      &master::MasterDdlProxy::IsDeleteNamespaceDoneAsync);
  // RETURN_NOT_OK macro can't take templated function call as param,
  // and SyncLeaderMasterRpc must be explicitly instantiated, else the
  // compiler complains.
  if (resp.has_error()) {
    if (resp.error().code() == MasterErrorPB::OBJECT_NOT_FOUND) {
      *delete_in_progress = false;
      return Status::OK();
    }
    return StatusFromPB(resp.error().status());
  }
  RETURN_NOT_OK(s);

  *delete_in_progress = !resp.done();
  return Status::OK();
}

Status YBClient::Data::WaitForDeleteNamespaceToFinish(YBClient* client,
    const NamespaceName& namespace_name,
    const std::optional<YQLDatabase>& database_type,
    const NamespaceId& namespace_id,
    CoarseTimePoint deadline) {
  return RetryUntilShutdown(
      deadline,
      "Waiting on Delete Namespace to be completed",
      "Timed out waiting for Namespace Deletion",
      std::bind(
          &YBClient::Data::IsDeleteNamespaceInProgress, this, client, namespace_name, database_type,
          namespace_id, _1, _2));
}

Status YBClient::Data::IsCloneNamespaceInProgress(
    YBClient* client, const std::string& source_namespace_id, uint32_t clone_seq_no,
    CoarseTimePoint deadline, bool* create_in_progress) {
  DCHECK_ONLY_NOTNULL(create_in_progress);
  ListClonesRequestPB req;
  ListClonesResponsePB resp;

  req.set_source_namespace_id(source_namespace_id);
  req.set_seq_no(clone_seq_no);
  // RETURN_NOT_OK macro can't take templated function call as param,
  // and SyncLeaderMasterRpc must be explicitly instantiated, else the
  // compiler complains.
  const Status s = SyncLeaderMasterRpc(
      deadline, req, &resp, "ListClones", &master::MasterBackupProxy::ListClonesAsync);

  RETURN_NOT_OK(s);
  // ListClones could return a terminal/done state as FAILED. This would result in an error'd
  // Status.
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  if (resp.entries_size() != 1) {
    return STATUS_FORMAT(InternalError, "Expected 1 clone entry, got $0", resp.entries_size());
  }
  auto state = resp.entries(0).aggregate_state();
  *create_in_progress =
      !(state == master::SysCloneStatePB::ABORTED || state == master::SysCloneStatePB::COMPLETE);

  if (state == master::SysCloneStatePB_State_ABORTED) {
    return STATUS_FORMAT(
        InternalError, "Clone operation aborted: $0", resp.entries(0).abort_message());
  }

  return Status::OK();
}

Status YBClient::Data::WaitForCloneNamespaceToFinish(
    YBClient* client, const std::string& source_namespace_id, uint32_t clone_seq_no,
    CoarseTimePoint deadline) {
  return RetryUntilShutdown(
      deadline, "Waiting on Clone Namespace to be completed",
      "Timed out waiting for Namespace Cloning",
      std::bind(
          &YBClient::Data::IsCloneNamespaceInProgress, this, client, source_namespace_id,
          clone_seq_no, _1, _2));
}

Status YBClient::Data::AlterTable(YBClient* client,
                                  const AlterTableRequestPB& req,
                                  CoarseTimePoint deadline) {
  AlterTableResponsePB resp;
  return SyncLeaderMasterRpc(
      deadline, req, &resp, "AlterTable", &master::MasterDdlProxy::AlterTableAsync);
}

Status YBClient::Data::IsAlterTableInProgress(
    YBClient* client,
    const YBTableName& table_name,
    const TableId& table_id,
    CoarseTimePoint deadline,
    bool *alter_in_progress) {
  IsAlterTableDoneRequestPB req;
  IsAlterTableDoneResponsePB resp;

  if (table_name.has_table()) {
    table_name.SetIntoTableIdentifierPB(req.mutable_table());
  }

  if (!table_id.empty()) {
    (req.mutable_table())->set_table_id(table_id);
  }

  RETURN_NOT_OK(SyncLeaderMasterRpc(
      deadline, req, &resp, "IsAlterTableDone",
      &master::MasterDdlProxy::IsAlterTableDoneAsync));

  *alter_in_progress = !resp.done();
  return Status::OK();
}

Status YBClient::Data::WaitForAlterTableToFinish(
    YBClient* client,
    const YBTableName& alter_name,
    const TableId& table_id,
    CoarseTimePoint deadline) {
  return RetryUntilShutdown(
      deadline, "Waiting on Alter Table to be completed", "Timed out waiting for AlterTable",
      std::bind(
          &YBClient::Data::IsAlterTableInProgress, this, client, alter_name, table_id, _1, _2));
}

Status YBClient::Data::FlushTablesHelper(YBClient* client,
                                         const CoarseTimePoint deadline,
                                         const FlushTablesRequestPB& req) {
  FlushTablesResponsePB resp;

  RETURN_NOT_OK(SyncLeaderMasterRpc(
      deadline, req, &resp, "FlushTables", &master::MasterAdminProxy::FlushTablesAsync));

  // Spin until the table is flushed.
  if (!resp.flush_request_id().empty()) {
    RETURN_NOT_OK(WaitForFlushTableToFinish(client, resp.flush_request_id(), deadline));
  }

  LOG(INFO) << (req.is_compaction() ? "Compacted" : "Flushed")
            << " table "
            << req.tables(0).ShortDebugString()
            << (req.add_indexes() ? " and indexes" : "");
  return Status::OK();
}

Status YBClient::Data::FlushOrCompactTables(
    YBClient* client, const vector<YBTableName>& table_names, bool add_indexes,
    bool add_vector_indexes, bool is_compaction, CoarseTimePoint deadline) {
  FlushTablesRequestPB req;
  req.set_add_indexes(add_indexes);
  req.set_is_compaction(is_compaction);
  for (const auto& table : table_names) {
    table.SetIntoTableIdentifierPB(req.add_tables());
  }
  if (add_vector_indexes) {
    req.set_flags(tablet::FLUSH_COMPACT_ALL);
  }

  return FlushTablesHelper(client, deadline, req);
}

Status YBClient::Data::FlushOrCompactTables(
    YBClient* client, const TableIds& table_ids, bool add_indexes,
    bool add_vector_indexes, bool is_compaction, CoarseTimePoint deadline) {
  FlushTablesRequestPB req;
  req.set_add_indexes(add_indexes);
  req.set_is_compaction(is_compaction);
  for (const auto& table : table_ids) {
    req.add_tables()->set_table_id(table);
  }
  if (add_vector_indexes) {
    req.set_flags(tablet::FLUSH_COMPACT_ALL);
  }

  return FlushTablesHelper(client, deadline, req);
}

Status YBClient::Data::IsFlushTableInProgress(YBClient* client,
                                              const FlushRequestId& flush_id,
                                              const CoarseTimePoint deadline,
                                              bool *flush_in_progress) {
  DCHECK_ONLY_NOTNULL(flush_in_progress);
  IsFlushTablesDoneRequestPB req;
  IsFlushTablesDoneResponsePB resp;

  req.set_flush_request_id(flush_id);
  RETURN_NOT_OK(SyncLeaderMasterRpc(
      deadline, req, &resp, "IsFlushTableDone",
      &master::MasterAdminProxy::IsFlushTablesDoneAsync));

  *flush_in_progress = !resp.done();
  if (!resp.success()) {
    return STATUS_FORMAT(InternalError, "Flush operation for id: $0 failed", flush_id);
  }
  return Status::OK();
}

Status YBClient::Data::WaitForFlushTableToFinish(YBClient* client,
                                                 const FlushRequestId& flush_id,
                                                 const CoarseTimePoint deadline) {
  return RetryUntilShutdown(
      deadline, "Waiting for FlushTables to be completed", "Timed out waiting for FlushTables",
      std::bind(&YBClient::Data::IsFlushTableInProgress, this, client, flush_id, _1, _2));
}

Result<TableCompactionStatus> YBClient::Data::GetCompactionStatus(
    const YBTableName& table_name, bool show_tablets, const CoarseTimePoint deadline) {
  GetCompactionStatusRequestPB req;
  GetCompactionStatusResponsePB resp;

  if (!table_name.has_table()) {
    const string msg = "Could not get the compaction status without the table name" +
                       (table_name.has_table_id() ? " for table id: " + table_name.table_id() : "");
    return STATUS(InvalidArgument, msg);
  }
  table_name.SetIntoTableIdentifierPB(req.mutable_table());

  if (show_tablets) {
    req.set_show_tablets(true);
  }

  RETURN_NOT_OK(SyncLeaderMasterRpc(
      deadline, req, &resp, "GetCompactionStatus",
      &master::MasterAdminProxy::GetCompactionStatusAsync));

  if (!resp.has_full_compaction_state()) {
    return STATUS(InternalError, "Missing full compaction state in table full compaction status");
  }

  std::vector<TabletReplicaFullCompactionStatus> replica_statuses;
  for (const auto& replica_status : resp.replica_statuses()) {
    if (!replica_status.has_ts_id() || !replica_status.has_tablet_id() ||
        !replica_status.has_full_compaction_state()) {
      return STATUS(InternalError, "Missing field(s) in tablet replica full compaction status");
    }

    replica_statuses.push_back(TabletReplicaFullCompactionStatus{
        .ts_id = replica_status.ts_id(),
        .tablet_id = replica_status.tablet_id(),
        .full_compaction_state = replica_status.full_compaction_state(),
        .last_full_compaction_time = HybridTime(replica_status.last_full_compaction_time())});
  }

  return TableCompactionStatus{
      .full_compaction_state = resp.full_compaction_state(),
      .last_full_compaction_time = HybridTime(resp.last_full_compaction_time()),
      .last_request_time = HybridTime(resp.last_request_time()),
      .replica_statuses = std::move(replica_statuses)};
}

bool YBClient::Data::IsTabletServerLocal(const RemoteTabletServer& rts) const {
  // If the uuid's are same, we are sure the tablet server is local, since if this client is used
  // via the CQL proxy, the tablet server's uuid is set in the client.
  return uuid_ == rts.permanent_uuid();
}

template <class T, class... Args>
rpc::RpcCommandPtr YBClient::Data::StartRpc(Args&&... args) {
  auto rpc = std::make_shared<T>(std::forward<Args>(args)...);
  rpcs_.RegisterAndStart(rpc, rpc->RpcHandle());
  return rpc;
}

namespace internal {

// Gets a table's schema from the leader master. See ClientMasterRpc.
class GetTableSchemaRpc
    : public ClientMasterRpc<GetTableSchemaRequestPB, GetTableSchemaResponsePB> {
 public:
  GetTableSchemaRpc(YBClient* client,
                    StatusCallback user_cb,
                    const YBTableName& table_name,
                    YBTableInfo* info,
                    CoarseTimePoint deadline,
                    master::IncludeHidden include_hidden = master::IncludeHidden::kFalse);
  GetTableSchemaRpc(YBClient* client,
                    StatusCallback user_cb,
                    TableIdView table_id,
                    YBTableInfo* info,
                    CoarseTimePoint deadline,
                    master::IncludeHidden include_hidden,
                    master::GetTableSchemaResponsePB* resp_copy);

  std::string ToString() const override;

  ~GetTableSchemaRpc() override;

 private:
  GetTableSchemaRpc(YBClient* client,
                    StatusCallback user_cb,
                    const master::TableIdentifierPB& table_identifier,
                    YBTableInfo* info,
                    CoarseTimePoint deadline,
                    master::IncludeHidden include_hidden = master::IncludeHidden::kFalse,
                    master::GetTableSchemaResponsePB* resp_copy = nullptr);

  void CallRemoteMethod() override;
  void ProcessResponse(const Status& status) override;

  StatusCallback user_cb_;
  master::TableIdentifierPB table_identifier_;
  YBTableInfo* info_;
  master::GetTableSchemaResponsePB* resp_copy_;
};

// Gets all table schemas for a tablegroup from the leader master. See ClientMasterRpc.
class GetTablegroupSchemaRpc
    : public ClientMasterRpc<GetTablegroupSchemaRequestPB, GetTablegroupSchemaResponsePB> {
 public:
  GetTablegroupSchemaRpc(YBClient* client,
                         StatusCallback user_cb,
                         const TablegroupId& tablegroup_id,
                         vector<YBTableInfo>* info,
                         CoarseTimePoint deadline);

  std::string ToString() const override;

  ~GetTablegroupSchemaRpc() override;

 private:
  GetTablegroupSchemaRpc(YBClient* client,
                         StatusCallback user_cb,
                         const master::TablegroupIdentifierPB& tablegroup,
                         vector<YBTableInfo>* info,
                         CoarseTimePoint deadline);

  void CallRemoteMethod() override;
  void ProcessResponse(const Status& status) override;

  StatusCallback user_cb_;
  master::TablegroupIdentifierPB tablegroup_identifier_;
  vector<YBTableInfo>* info_;
};

namespace {

master::TableIdentifierPB ToTableIdentifierPB(const YBTableName& table_name) {
  master::TableIdentifierPB id;
  table_name.SetIntoTableIdentifierPB(&id);
  return id;
}

master::TableIdentifierPB ToTableIdentifierPB(TableIdView table_id) {
  master::TableIdentifierPB id;
  id.set_table_id(table_id.data(), table_id.size());
  return id;
}

master::TablegroupIdentifierPB ToTablegroupIdentifierPB(const TablegroupId& tablegroup_id) {
  DCHECK(IsIdLikeUuid(tablegroup_id)) << tablegroup_id;
  master::TablegroupIdentifierPB id;
  id.set_id(tablegroup_id);
  return id;
}

} // namespace
} // namespace internal

// Helper function to create YBTableInfo from GetTableSchemaResponsePB.
Status CreateTableInfoFromTableSchemaResp(const GetTableSchemaResponsePB& resp, YBTableInfo* info) {
  std::unique_ptr<Schema> schema = std::make_unique<Schema>(Schema());
  RETURN_NOT_OK(SchemaFromPB(resp.schema(), schema.get()));
  info->schema.Reset(std::move(schema));
  info->schema.set_version(resp.version());
  info->schema.set_is_compatible_with_previous_version(
      resp.is_compatible_with_previous_version());
  RETURN_NOT_OK(dockv::PartitionSchema::FromPB(
      resp.partition_schema(), internal::GetSchema(&info->schema), &info->partition_schema));

  info->table_name.GetFromTableIdentifierPB(resp.identifier());
  if (!resp.schema().deprecated_pgschema_name().empty()) {
    info->table_name.set_pgschema_name(resp.schema().deprecated_pgschema_name());
  }
  info->table_id = resp.identifier().table_id();
  info->table_type = VERIFY_RESULT(PBToClientTableType(resp.table_type()));
  info->index_map.FromPB(resp.indexes());
  if (resp.has_index_info()) {
    info->index_info.emplace(resp.index_info());
  }
  if (resp.has_replication_info()) {
    info->replication_info.emplace(resp.replication_info());
  }
  if (resp.has_wal_retention_secs()) {
    info->wal_retention_secs = resp.wal_retention_secs();
  }
  if (resp.ysql_ddl_txn_verifier_state_size() > 0) {
    info->ysql_ddl_txn_verifier_state.emplace(resp.ysql_ddl_txn_verifier_state());
  }
  SCHECK_GT(info->table_id.size(), 0U, IllegalState, "Running against a too-old master");
  info->colocated = resp.colocated();

  info->pg_table_id = resp.pg_table_id();

  return Status::OK();
}

namespace internal {

GetTableSchemaRpc::GetTableSchemaRpc(YBClient* client,
                                     StatusCallback user_cb,
                                     const YBTableName& table_name,
                                     YBTableInfo* info,
                                     CoarseTimePoint deadline,
                                     master::IncludeHidden include_hidden)
    : GetTableSchemaRpc(
          client, user_cb, ToTableIdentifierPB(table_name), info, deadline, include_hidden) {
}

GetTableSchemaRpc::GetTableSchemaRpc(YBClient* client,
                                     StatusCallback user_cb,
                                     TableIdView table_id,
                                     YBTableInfo* info,
                                     CoarseTimePoint deadline,
                                     master::IncludeHidden include_hidden,
                                     master::GetTableSchemaResponsePB* resp_copy)
    : GetTableSchemaRpc(
          client, user_cb, ToTableIdentifierPB(table_id), info, deadline, include_hidden,
          resp_copy) {}

GetTableSchemaRpc::GetTableSchemaRpc(YBClient* client,
                                     StatusCallback user_cb,
                                     const master::TableIdentifierPB& table_identifier,
                                     YBTableInfo* info,
                                     CoarseTimePoint deadline,
                                     master::IncludeHidden include_hidden,
                                     master::GetTableSchemaResponsePB* resp_copy)
    : ClientMasterRpc(client, deadline),
      user_cb_(std::move(user_cb)),
      table_identifier_(table_identifier),
      info_(info),
      resp_copy_(resp_copy) {
  req_.mutable_table()->CopyFrom(table_identifier_);
  req_.set_include_hidden(include_hidden);
  req_.set_check_exists_only(info == nullptr);
}

GetTableSchemaRpc::~GetTableSchemaRpc() {
}

void GetTableSchemaRpc::CallRemoteMethod() {
  master_ddl_proxy()->GetTableSchemaAsync(
      req_, &resp_, mutable_retrier()->mutable_controller(),
      std::bind(&GetTableSchemaRpc::Finished, this, Status::OK()));
}

string GetTableSchemaRpc::ToString() const {
  return Format("GetTableSchemaRpc(table_identifier: $0, num_attempts: $1, check_only: $2)",
                table_identifier_.ShortDebugString(), num_attempts(), info_ == nullptr);
}

void GetTableSchemaRpc::ProcessResponse(const Status& status) {
  auto new_status = status;
  if (new_status.ok()) {
    if (info_) {
      new_status = CreateTableInfoFromTableSchemaResp(resp_, info_);
    }
    if (resp_copy_) {
      resp_copy_->Swap(&resp_);
    }
  }
  if (!new_status.ok()) {
    LOG(WARNING) << ToString() << " failed: " << new_status.ToString();
  }
  user_cb_.Run(new_status);
}

GetTablegroupSchemaRpc::GetTablegroupSchemaRpc(
    YBClient* client,
    StatusCallback user_cb,
    const TablegroupId& tablegroup_id,
    vector<YBTableInfo>* info,
    CoarseTimePoint deadline)
    : ClientMasterRpc(client, deadline),
      user_cb_(std::move(user_cb)),
      tablegroup_identifier_(ToTablegroupIdentifierPB(tablegroup_id)),
      info_(DCHECK_NOTNULL(info)) {
  req_.mutable_tablegroup()->CopyFrom(tablegroup_identifier_);
}

GetTablegroupSchemaRpc::GetTablegroupSchemaRpc(
    YBClient* client,
    StatusCallback user_cb,
    const master::TablegroupIdentifierPB& tablegroup_identifier,
    vector<YBTableInfo>* info,
    CoarseTimePoint deadline)
    : ClientMasterRpc(client, deadline),
      user_cb_(std::move(user_cb)),
      tablegroup_identifier_(tablegroup_identifier),
      info_(DCHECK_NOTNULL(info)) {
  req_.mutable_tablegroup()->CopyFrom(tablegroup_identifier_);
}

GetTablegroupSchemaRpc::~GetTablegroupSchemaRpc() {
}

void GetTablegroupSchemaRpc::CallRemoteMethod() {
  master_ddl_proxy()->GetTablegroupSchemaAsync(
      req_, &resp_, mutable_retrier()->mutable_controller(),
      std::bind(&GetTablegroupSchemaRpc::Finished, this, Status::OK()));
}

string GetTablegroupSchemaRpc::ToString() const {
  return Format("GetTablegroupSchemaRpc(table_identifier: $0, num_attempts: $1",
                tablegroup_identifier_.ShortDebugString(), num_attempts());
}

void GetTablegroupSchemaRpc::ProcessResponse(const Status& status) {
  auto new_status = status;
  if (new_status.ok()) {
    for (const auto& resp : resp_.get_table_schema_response_pbs()) {
      info_->emplace_back();
      new_status = CreateTableInfoFromTableSchemaResp(resp, &info_->back());
      if (!new_status.ok()) {
        break;
      }
    }
  }
  if (!new_status.ok()) {
    LOG(WARNING) << ToString() << " failed: " << new_status.ToString();
  }
  user_cb_.Run(new_status);
}

class CreateXClusterStreamRpc
    : public ClientMasterRpc<CreateCDCStreamRequestPB, CreateCDCStreamResponsePB> {
 public:
  CreateXClusterStreamRpc(
      YBClient* client, CreateCDCStreamCallback user_cb, const TableId& table_id,
      const google::protobuf::RepeatedPtrField<yb::master::CDCStreamOptionsPB>& options,
      master::SysCDCStreamEntryPB::State state, cdc::StreamModeTransactional transactional,
      CoarseTimePoint deadline);

  string ToString() const override;

  ~CreateXClusterStreamRpc() override;

 private:
  void CallRemoteMethod() override;
  void ProcessResponse(const Status& status) override;

  const CreateCDCStreamCallback user_cb_;
  const std::string table_id_;
};

CreateXClusterStreamRpc::CreateXClusterStreamRpc(
    YBClient* client, CreateCDCStreamCallback user_cb, const TableId& table_id,
    const google::protobuf::RepeatedPtrField<yb::master::CDCStreamOptionsPB>& options,
    master::SysCDCStreamEntryPB::State state, cdc::StreamModeTransactional transactional,
    CoarseTimePoint deadline)
    : ClientMasterRpc(client, deadline), user_cb_(std::move(user_cb)), table_id_(table_id) {
  req_.set_table_id(table_id_);
  // (DEPRECATE_EOL 2024.1) Master now sets the options explicitly, so they do not need to be sent.
  *req_.mutable_options() = options;
  req_.set_transactional(transactional);
  req_.set_initial_state(state);
}

CreateXClusterStreamRpc::~CreateXClusterStreamRpc() {}

void CreateXClusterStreamRpc::CallRemoteMethod() {
  master_replication_proxy()->CreateCDCStreamAsync(
      req_, &resp_, mutable_retrier()->mutable_controller(),
      std::bind(&CreateXClusterStreamRpc::Finished, this, Status::OK()));
}

string CreateXClusterStreamRpc::ToString() const {
  return Format("CreateXClusterStream(table_id: $0, num_attempts: $1)", table_id_, num_attempts());
}

void CreateXClusterStreamRpc::ProcessResponse(const Status& status) {
  if (status.ok()) {
    user_cb_(xrepl::StreamId::FromString(resp_.stream_id()));
  } else {
    LOG(WARNING) << ToString() << " failed: " << status.ToString();
    user_cb_(status);
  }
}

class DeleteCDCStreamRpc
    : public ClientMasterRpc<DeleteCDCStreamRequestPB, DeleteCDCStreamResponsePB> {
 public:
  DeleteCDCStreamRpc(
      YBClient* client,
      StatusCallback user_cb,
      const xrepl::StreamId& stream_id,
      CoarseTimePoint deadline);

  string ToString() const override;

  ~DeleteCDCStreamRpc() override;

 private:
  void CallRemoteMethod() override;
  void ProcessResponse(const Status& status) override;

  StatusCallback user_cb_;
  xrepl::StreamId stream_id_;
};

DeleteCDCStreamRpc::DeleteCDCStreamRpc(
    YBClient* client,
    StatusCallback user_cb,
    const xrepl::StreamId& stream_id,
    CoarseTimePoint deadline)
    : ClientMasterRpc(client, deadline), user_cb_(std::move(user_cb)), stream_id_(stream_id) {
  req_.add_stream_id(stream_id_.ToString());
}

DeleteCDCStreamRpc::~DeleteCDCStreamRpc() {
}

void DeleteCDCStreamRpc::CallRemoteMethod() {
  master_replication_proxy()->DeleteCDCStreamAsync(
      req_, &resp_, mutable_retrier()->mutable_controller(),
      std::bind(&DeleteCDCStreamRpc::Finished, this, Status::OK()));
}

string DeleteCDCStreamRpc::ToString() const {
  return Format("DeleteCDCStream(stream_id: $0, num_attempts: $1)", stream_id_, num_attempts());
}

void DeleteCDCStreamRpc::ProcessResponse(const Status& status) {
  if (!status.ok()) {
    LOG(WARNING) << ToString() << " failed: " << status.ToString();
  }
  user_cb_.Run(status);
}

class CreateSnapshotRpc
    : public ClientMasterRpc<CreateSnapshotRequestPB, CreateSnapshotResponsePB> {
 public:
  CreateSnapshotRpc(YBClient* client, CreateSnapshotCallback user_cb, CoarseTimePoint deadline)
      : ClientMasterRpc(client, deadline), user_cb_(std::move(user_cb)) {}

  Status Init(const std::vector<client::YBTableName>& tables) {
    SCHECK(!tables.empty(), InvalidArgument, "Table names is empty");

    for (const auto& table : tables) {
      master::TableIdentifierPB id;
      table.SetIntoTableIdentifierPB(&id);
      req_.mutable_tables()->Add()->Swap(&id);
    }
    return Status::OK();
  }

  string ToString() const override {
    return Format("CreateSnapshotRpc(num_attempts: $0)", num_attempts());
  }

  ~CreateSnapshotRpc() override {}

 private:
  void CallRemoteMethod() override {
    master_backup_proxy()->CreateSnapshotAsync(
        req_, &resp_, mutable_retrier()->mutable_controller(),
        std::bind(&CreateSnapshotRpc::Finished, this, Status::OK()));
  }

  void ProcessResponse(const Status& status) override {
    if (!status.ok()) {
      LOG(WARNING) << ToString() << " failed: " << status.ToString();
      user_cb_(status);
      return;
    }

    auto result = ProcessResponseInternal();
    if (!result) {
      LOG(WARNING) << ToString() << " failed: " << result.status().ToString();
    }

    user_cb_(std::move(result));
  }

  Result<TxnSnapshotId> ProcessResponseInternal() {
    if (resp_.has_error()) {
      return StatusFromPB(resp_.error().status());
    }

    SCHECK(
        resp_.has_snapshot_id() && !resp_.snapshot_id().empty(), IllegalState,
        "Expected non-empty snapshot_id from response");

    return FullyDecodeTxnSnapshotId(resp_.snapshot_id());
  }

  CreateSnapshotCallback user_cb_;
};

class BootstrapProducerRpc
    : public ClientMasterRpc<
          master::BootstrapProducerRequestPB, master::BootstrapProducerResponsePB> {
 public:
  BootstrapProducerRpc(
      YBClient* client, BootstrapProducerCallback user_cb, CoarseTimePoint deadline)
      : ClientMasterRpc(client, deadline), user_cb_(std::move(user_cb)) {}

  Status Init(
      const YQLDatabase& db_type,
      const NamespaceName& namespace_name,
      const std::vector<PgSchemaName>& pg_schema_names,
      const std::vector<TableName>& table_names) {
    SCHECK(!namespace_name.empty(), InvalidArgument, "Table namespace name is empty");
    SCHECK(!table_names.empty(), InvalidArgument, "Table names is empty");
    table_names_ = table_names;
    tables_count_ = table_names.size();

    if (db_type == YQL_DATABASE_PGSQL) {
      SCHECK_EQ(
          pg_schema_names.size(), tables_count_, InvalidArgument,
          "Number of tables and PG schemas must match");
    } else {
      SCHECK(pg_schema_names.empty(), InvalidArgument, "PG Schema only applies to PG databases");
    }

    req_.set_db_type(db_type);
    req_.set_namespace_name(namespace_name);
    for (size_t i = 0; i < tables_count_; i++) {
      SCHECK(!table_names[i].empty(), InvalidArgument, "Table name is empty");
      req_.add_table_name(table_names[i]);
      if (db_type == YQL_DATABASE_PGSQL) {
        SCHECK(
            !pg_schema_names[i].empty(), InvalidArgument, "Table schema name at index $0 is empty",
            i);
        req_.add_pg_schema_name(pg_schema_names[i]);
      }
    }

    return Status::OK();
  }

  string ToString() const override {
    return Format(
        "BootstrapProducerRpc(table_names: $0, num_attempts: $1)", yb::ToString(table_names_),
        num_attempts());
  }

  ~BootstrapProducerRpc() override {}

 private:
  void CallRemoteMethod() override {
    master_replication_proxy()->BootstrapProducerAsync(
        req_, &resp_, mutable_retrier()->mutable_controller(),
        std::bind(&BootstrapProducerRpc::Finished, this, Status::OK()));
  }

  void ProcessResponse(const Status& status) override {
    if (!status.ok()) {
      LOG(WARNING) << ToString() << " failed: " << status.ToString();
      user_cb_(status);
      return;
    }

    auto result = ProcessResponseInternal();
    if (!result) {
      LOG(WARNING) << ToString() << " failed: " << result.status().ToString();
    }

    user_cb_(std::move(result));
  }

  BootstrapProducerResult ProcessResponseInternal() {
    if (resp_.has_error()) {
      return StatusFromPB(resp_.error().status());
    }

    SCHECK_EQ(
        resp_.table_ids_size(), narrow_cast<int>(tables_count_), IllegalState,
        "Unexpected number of results received");
    std::vector<std::string> producer_table_ids{resp_.table_ids().begin(), resp_.table_ids().end()};

    SCHECK_EQ(
        resp_.bootstrap_ids_size(), narrow_cast<int>(tables_count_), IllegalState,
        Format("Expected $0 results, received: $1", tables_count_, resp_.bootstrap_ids_size()));
    std::vector<std::string> bootstrap_ids{
        resp_.bootstrap_ids().begin(), resp_.bootstrap_ids().end()};

    HybridTime bootstrap_time = HybridTime::kInvalid;
    if (resp_.has_bootstrap_time()) {
      bootstrap_time = HybridTime(resp_.bootstrap_time());
    }

    return std::make_tuple(std::move(producer_table_ids), std::move(bootstrap_ids), bootstrap_time);
  }

 private:
  BootstrapProducerCallback user_cb_;
  std::vector<TableName> table_names_;
  size_t tables_count_ = 0;
};

class GetCDCDBStreamInfoRpc : public ClientMasterRpc<GetCDCDBStreamInfoRequestPB,
                                                     GetCDCDBStreamInfoResponsePB> {
 public:
  GetCDCDBStreamInfoRpc(
      YBClient* client,
      StdStatusCallback user_cb,
      const std::string& db_stream_id,
      std::vector<pair<std::string, std::string>>* db_stream_info,
      CoarseTimePoint deadline);

  std::string ToString() const override;

  ~GetCDCDBStreamInfoRpc() override = default;

 private:
  void CallRemoteMethod() override;
  void ProcessResponse(const Status& status) override;

  StdStatusCallback user_cb_;
  std::string db_stream_id_;
  std::vector<pair<std::string, std::string>>* db_stream_info_;
};

GetCDCDBStreamInfoRpc::GetCDCDBStreamInfoRpc(YBClient *client,
  StdStatusCallback user_cb,
  const std::string &db_stream_id,
  std::vector<pair<std::string, std::string>> *db_stream_info,
  CoarseTimePoint deadline)
  : ClientMasterRpc(client, deadline),
    user_cb_(std::move(user_cb)),
    db_stream_id_(db_stream_id),
    db_stream_info_(DCHECK_NOTNULL(db_stream_info)) {
  req_.set_db_stream_id(db_stream_id_);
}

void GetCDCDBStreamInfoRpc::CallRemoteMethod() {
  master_replication_proxy()->GetCDCDBStreamInfoAsync(
      req_, &resp_, mutable_retrier()->mutable_controller(),
      std::bind(&GetCDCDBStreamInfoRpc::Finished, this, Status::OK()));
}

string GetCDCDBStreamInfoRpc::ToString() const {
  return Format("GetCDCDBStreamInfo(db_stream_id: $0, num_attempts: $1)",
                db_stream_id_, num_attempts());
}

void GetCDCDBStreamInfoRpc::ProcessResponse(const Status& status) {
  if (!status.ok()) {
    LOG(WARNING) << ToString() << " failed: " << status.ToString();
  } else {
    db_stream_info_->clear();
    db_stream_info_->reserve(resp_.table_info_size());
    for (const auto& table_info : resp_.table_info()) {
      db_stream_info_->push_back(std::make_pair(table_info.stream_id(), table_info.table_id()));
    }
  }
  user_cb_(status);
}

class GetCDCStreamRpc : public ClientMasterRpc<GetCDCStreamRequestPB, GetCDCStreamResponsePB> {
 public:
  GetCDCStreamRpc(
      YBClient* client,
      StdStatusCallback user_cb,
      const xrepl::StreamId& stream_id,
      ObjectId* object_id,
      std::unordered_map<std::string, std::string>* options,
      CoarseTimePoint deadline,
      bool* xcluster_use_target_applied_filter);

  std::string ToString() const override;

  ~GetCDCStreamRpc() override;

 private:
  void CallRemoteMethod() override;
  void ProcessResponse(const Status& status) override;

  StdStatusCallback user_cb_;
  xrepl::StreamId stream_id_;
  ObjectId* object_id_;
  std::unordered_map<std::string, std::string>* options_;
  bool* xcluster_use_target_applied_filter_;
};

GetCDCStreamRpc::GetCDCStreamRpc(
    YBClient* client,
    StdStatusCallback user_cb,
    const xrepl::StreamId& stream_id,
    TableId* object_id,
    std::unordered_map<std::string, std::string>* options,
    CoarseTimePoint deadline,
    bool* xcluster_use_target_applied_filter)
    : ClientMasterRpc(client, deadline),
      user_cb_(std::move(user_cb)),
      stream_id_(stream_id),
      object_id_(DCHECK_NOTNULL(object_id)),
      options_(DCHECK_NOTNULL(options)),
      xcluster_use_target_applied_filter_(xcluster_use_target_applied_filter) {
  req_.set_stream_id(stream_id_.ToString());
}

GetCDCStreamRpc::~GetCDCStreamRpc() {
}

void GetCDCStreamRpc::CallRemoteMethod() {
  master_replication_proxy()->GetCDCStreamAsync(
      req_, &resp_, mutable_retrier()->mutable_controller(),
      std::bind(&GetCDCStreamRpc::Finished, this, Status::OK()));
}

string GetCDCStreamRpc::ToString() const {
  return Format("GetCDCStream(stream_id: $0, num_attempts: $1)", stream_id_, num_attempts());
}

void GetCDCStreamRpc::ProcessResponse(const Status& status) {
  if (!status.ok()) {
    LOG(WARNING) << ToString() << " failed: " << status.ToString();
  } else {
    if (resp_.stream().has_namespace_id()) {
      *object_id_ = resp_.stream().namespace_id();
    } else {
      *object_id_ = resp_.stream().table_id().Get(0);
    }

    options_->clear();
    options_->reserve(resp_.stream().options_size());
    for (const auto& option : resp_.stream().options()) {
      options_->emplace(option.key(), option.value());
    }

    if (xcluster_use_target_applied_filter_) {
      *xcluster_use_target_applied_filter_ = resp_.stream().xcluster_use_target_applied_filter();
    }
  }
  user_cb_(status);
}

class DeleteNotServingTabletRpc
    : public ClientMasterRpc<
          master::DeleteNotServingTabletRequestPB, master::DeleteNotServingTabletResponsePB> {
 public:
  DeleteNotServingTabletRpc(
      YBClient* client,
      const TabletId& tablet_id,
      StdStatusCallback user_cb,
      CoarseTimePoint deadline)
      : ClientMasterRpc(client, deadline),
        user_cb_(std::move(user_cb)) {
    req_.set_tablet_id(tablet_id);
  }

  std::string ToString() const override {
    return Format(
        "DeleteNotServingTabletRpc(tablet_id: $0, num_attempts: $1)", req_.tablet_id(),
        num_attempts());
  }

  ~DeleteNotServingTabletRpc() override = default;

 private:
  void CallRemoteMethod() override {
    master_admin_proxy()->DeleteNotServingTabletAsync(
        req_, &resp_, mutable_retrier()->mutable_controller(),
        std::bind(&DeleteNotServingTabletRpc::Finished, this, Status::OK()));
  }

  void ProcessResponse(const Status& status) override {
    if (!status.ok()) {
      LOG(WARNING) << ToString() << " failed: " << status.ToString();
    }
    user_cb_(status);
  }

  StdStatusCallback user_cb_;
};

class GetTableLocationsRpc
    : public ClientMasterRpc<
          master::GetTableLocationsRequestPB, master::GetTableLocationsResponsePB> {
 public:
  GetTableLocationsRpc(
      YBClient* client, const TableId& table_id, int32_t max_tablets,
      RequireTabletsRunning require_tablets_running, PartitionsOnly partitions_only,
      GetTableLocationsCallback user_cb, CoarseTimePoint deadline)
      : ClientMasterRpc(client, deadline), user_cb_(std::move(user_cb)) {
    req_.mutable_table()->set_table_id(table_id);
    req_.set_max_returned_locations(max_tablets);
    req_.set_require_tablets_running(require_tablets_running);
    req_.set_partitions_only(partitions_only);
  }

  std::string ToString() const override {
    return Format(
        "GetTableLocationsRpc(table_id: $0, max_tablets: $1, require_tablets_running: $2, "
        "num_attempts: $3,", req_.table().table_id(), req_.max_returned_locations(),
        req_.require_tablets_running(), num_attempts());
  }

  ~GetTableLocationsRpc() override = default;

 private:
  void CallRemoteMethod() override {
    master_client_proxy()->GetTableLocationsAsync(
        req_, &resp_, mutable_retrier()->mutable_controller(),
        std::bind(&GetTableLocationsRpc::Finished, this, Status::OK()));
  }

  bool ShouldRetry(const Status& status) override {
    if (status.IsShutdownInProgress() || status.IsNotFound() || status.IsAborted()) {
      // Return without retry in case of permanent errors.
      // We can get:
      // - ShutdownInProgress when catalog manager is in process of shutting down.
      // - Aborted when client is shutting down.
      // - NotFound when table has been deleted.
      LOG(WARNING) << ToString() << " failed: " << status;
      return false;
    }
    if (!status.ok()) {
      YB_LOG_EVERY_N_SECS(WARNING, 10)
          << ToString() << ": error getting table locations: " << status << ", retrying.";
      return true;
    }
    if (resp_.tablet_locations_size() > 0) {
      return false;
    }

    YB_LOG_EVERY_N_SECS(WARNING, 10) << ToString() << ": got zero table locations, retrying.";
    return true;
  }

  void ProcessResponse(const Status& status) override {
    if (status.ok()) {
      user_cb_(&resp_);
    } else {
      user_cb_(status);
    }
  }

  GetTableLocationsCallback user_cb_;
};

// A simple RPC that keeps retrying if resp has not_ready set.
class GetXClusterStreamsRpc
    : public ClientMasterRpc<
          master::GetXClusterStreamsRequestPB, master::GetXClusterStreamsResponsePB> {
 public:
  GetXClusterStreamsRpc(
      YBClient* client, std::function<void(Result<master::GetXClusterStreamsResponsePB>)> user_cb,
      CoarseTimePoint deadline)
      : ClientMasterRpc(client, deadline), user_cb_(std::move(user_cb)) {}

  Status Init(
      const xcluster::ReplicationGroupId& replication_group_id, const NamespaceId& namespace_id,
      const std::vector<TableName>& table_names, const std::vector<PgSchemaName>& pg_schema_names) {
    SCHECK_EQ(
        table_names.size(), pg_schema_names.size(), InvalidArgument,
        "Table names and pg schema names must be of the same size");

    req_.set_replication_group_id(replication_group_id.ToString());
    req_.set_namespace_id(namespace_id);

    for (size_t i = 0; i < table_names.size(); i++) {
      auto* table_info = req_.add_table_infos();
      table_info->set_table_name(table_names[i]);
      table_info->set_pg_schema_name(pg_schema_names[i]);
    }

    return Status::OK();
  }

  Status Init(
      const xcluster::ReplicationGroupId& replication_group_id, const NamespaceId& namespace_id,
      const std::vector<TableId>& source_table_ids, bool create_stream_if_missing) {
    req_.set_replication_group_id(replication_group_id.ToString());
    req_.set_namespace_id(namespace_id);
    for (const auto& table_id : source_table_ids) {
      req_.add_source_table_ids(table_id);
    }
    req_.set_create_stream_if_missing(create_stream_if_missing);
    return Status::OK();
  }

  string ToString() const override {
    return Format(
        "GetXClusterStreamsRpc(replication_group_id: $0, num_attempts: $1)",
        req_.replication_group_id(), num_attempts());
  }

  ~GetXClusterStreamsRpc() override {}

 private:
  void CallRemoteMethod() override {
    master_replication_proxy()->GetXClusterStreamsAsync(
        req_, &resp_, mutable_retrier()->mutable_controller(),
        std::bind(&GetXClusterStreamsRpc::Finished, this, Status::OK()));
  }

  Status ResponseStatus() override {
    RETURN_NOT_OK(internal::StatusFromResp(resp_));
    if (resp_.not_ready()) {
      VLOG(2) << ToString() << ": xClusterOutboundReplicationGroup is not ready yet, retrying.";
      return STATUS(TryAgain, "xClusterOutboundReplicationGroup is not ready yet");
    }
    return Status::OK();
  }

  bool ShouldRetry(const Status& status) override { return status.IsTryAgain(); }

  void ProcessResponse(const Status& status) override {
    if (!status.ok()) {
      LOG(WARNING) << ToString() << " failed: " << status;
      user_cb_(std::move(status));
      return;
    }

    user_cb_(std::move(resp_));
  }

  std::function<void(Result<master::GetXClusterStreamsResponsePB>)> user_cb_;
};

// A simple RPC that keeps retrying if resp has not_ready set.
class IsXClusterBootstrapRequiredRpc : public ClientMasterRpc<
                                           master::IsXClusterBootstrapRequiredRequestPB,
                                           master::IsXClusterBootstrapRequiredResponsePB> {
 public:
  IsXClusterBootstrapRequiredRpc(
      YBClient* client, std::function<void(Result<bool>)> user_cb, CoarseTimePoint deadline)
      : ClientMasterRpc(client, deadline), user_cb_(std::move(user_cb)) {}

  Status Init(
      const xcluster::ReplicationGroupId& replication_group_id, const NamespaceId& namespace_id) {
    req_.set_replication_group_id(replication_group_id.ToString());
    req_.set_namespace_id(namespace_id);
    return Status::OK();
  }

  string ToString() const override {
    return Format(
        "IsXClusterBootstrapRequired(replication_group_id: $0, num_attempts: $1)",
        req_.replication_group_id(), num_attempts());
  }

  ~IsXClusterBootstrapRequiredRpc() override {}

 private:
  void CallRemoteMethod() override {
    master_replication_proxy()->IsXClusterBootstrapRequiredAsync(
        req_, &resp_, mutable_retrier()->mutable_controller(),
        std::bind(&IsXClusterBootstrapRequiredRpc::Finished, this, Status::OK()));
  }

  Status ResponseStatus() override {
    RETURN_NOT_OK(internal::StatusFromResp(resp_));
    if (resp_.not_ready()) {
      VLOG(2) << ToString() << ": xClusterOutboundReplicationGroup is not ready yet, retrying.";
      return STATUS(TryAgain, "xClusterOutboundReplicationGroup is not ready yet");
    }
    return Status::OK();
  }

  bool ShouldRetry(const Status& status) override { return status.IsTryAgain(); }

  void ProcessResponse(const Status& status) override {
    if (!status.ok()) {
      LOG(WARNING) << ToString() << " failed: " << status;
      user_cb_(std::move(status));
      return;
    }

    user_cb_(resp_.initial_bootstrap_required());
  }

  std::function<void(Result<bool>)> user_cb_;
};

class AcquireObjectLocksGlobalRpc
    : public ClientMasterRpc<
          master::AcquireObjectLocksGlobalRequestPB, master::AcquireObjectLocksGlobalResponsePB> {
 public:
  AcquireObjectLocksGlobalRpc(
      YBClient* client, master::AcquireObjectLocksGlobalRequestPB request,
      StdStatusCallback user_cb, CoarseTimePoint deadline, std::function<Status()>&& should_retry)
      : ClientMasterRpc(client, deadline), user_cb_(std::move(user_cb)),
        should_retry_(std::move(should_retry)) {
    req_.CopyFrom(request);
  }

  string ToString() const override {
    return Format(
        "AcquireObjectLocksGlobal (request: $0, num_attempts: $1)", req_.DebugString(),
        num_attempts());
  }

  ~AcquireObjectLocksGlobalRpc() override {}

 private:
  Status ResponseStatus() override {
    return StatusFromResp(resp_);
  }

  bool ShouldRetry(const Status& status) override {
    if (!status.IsTryAgain()) {
      return false;
    }
    if (auto s = should_retry_(); !s.ok()) {
      error_ = s;
      return false;
    }
    return true;
  }

  void CallRemoteMethod() override {
    VLOG(2) << "Calling AcquireObjectLocksGlobal with request: " << req_.DebugString();
    master_ddl_proxy()->AcquireObjectLocksGlobalAsync(
        req_, &resp_, mutable_retrier()->mutable_controller(),
        std::bind(&AcquireObjectLocksGlobalRpc::Finished, this, Status::OK()));
  }

  void ProcessResponse(const Status& status) override {
    if (!status.ok()) {
      LOG(WARNING) << ToString() << " failed: " << status.ToString();
    }
    user_cb_(error_.ok() ? status : status.CloneAndAppend(error_.message()));
  }

  StdStatusCallback user_cb_;
  std::function<Status()> should_retry_;
  Status error_ = Status::OK();
};

class ReleaseObjectLocksGlobalRpc
    : public ClientMasterRpc<
          master::ReleaseObjectLocksGlobalRequestPB, master::ReleaseObjectLocksGlobalResponsePB> {
 public:
  ReleaseObjectLocksGlobalRpc(
      YBClient* client, master::ReleaseObjectLocksGlobalRequestPB request,
      StdStatusCallback user_cb, CoarseTimePoint deadline)
      : ClientMasterRpc(client, deadline), user_cb_(std::move(user_cb)) {
    req_.CopyFrom(request);
  }

  string ToString() const override {
    return Format(
        "ReleaseObjectLocksGlobal (request: $0, num_attempts: $1)", req_.DebugString(),
        num_attempts());
  }

  ~ReleaseObjectLocksGlobalRpc() override {}

 private:
  Status ResponseStatus() override {
    return StatusFromResp(resp_);
  }

  bool ShouldRetry(const Status& status) override {
    return status.IsTryAgain();
  }

  void CallRemoteMethod() override {
    VLOG(2) << "Calling ReleaseObjectLocksGlobalAsync with request: " << req_.DebugString();
    master_ddl_proxy()->ReleaseObjectLocksGlobalAsync(
        req_, &resp_, mutable_retrier()->mutable_controller(),
        std::bind(&ReleaseObjectLocksGlobalRpc::Finished, this, Status::OK()));
  }

  void ProcessResponse(const Status& status) override {
    if (!status.ok()) {
      LOG(WARNING) << ToString() << " failed: " << status.ToString();
    }
    user_cb_(status);
  }

  StdStatusCallback user_cb_;
};

class WaitForLockersMultipleGlobalRpc
    : public ClientMasterRpc<
          master::WaitForLockersMultipleGlobalRequestPB,
          master::WaitForLockersMultipleGlobalResponsePB> {
 public:
  WaitForLockersMultipleGlobalRpc(
      YBClient* client, master::WaitForLockersMultipleGlobalRequestPB request,
      StdStatusCallback user_cb, CoarseTimePoint deadline)
      : ClientMasterRpc(client, deadline), user_cb_(std::move(user_cb)) {
    req_.CopyFrom(request);
  }

  string ToString() const override {
    return Format(
        "WaitForLockersMultipleGlobal (request: $0, num_attempts: $1)", req_.DebugString(),
        num_attempts());
  }

  ~WaitForLockersMultipleGlobalRpc() override {}

 private:
  Status ResponseStatus() override {
    return StatusFromResp(resp_);
  }

  void CallRemoteMethod() override {
    VLOG(2) << "Calling WaitForLockersMultipleGlobal with request: " << req_.DebugString();
    master_ddl_proxy()->WaitForLockersMultipleGlobalAsync(
        req_, &resp_, mutable_retrier()->mutable_controller(),
        std::bind(&WaitForLockersMultipleGlobalRpc::Finished, this, Status::OK()));
  }

  void ProcessResponse(const Status& status) override {
    if (!status.ok()) {
      LOG(WARNING) << ToString() << " failed: " << status.ToString();
    }
    user_cb_(status);
  }

  StdStatusCallback user_cb_;
};

} // namespace internal

Status YBClient::Data::GetTableSchema(YBClient* client,
                                      const YBTableName& table_name,
                                      CoarseTimePoint deadline,
                                      YBTableInfo* info) {
  Synchronizer sync;
  auto rpc = StartRpc<GetTableSchemaRpc>(
      client,
      sync.AsStatusCallback(),
      table_name,
      info,
      deadline);
  return sync.Wait();
}

Status YBClient::Data::GetTableSchema(YBClient* client,
                                      TableIdView table_id,
                                      CoarseTimePoint deadline,
                                      YBTableInfo* info,
                                      master::IncludeHidden include_hidden,
                                      master::GetTableSchemaResponsePB* resp) {
  Synchronizer sync;
  auto rpc = StartRpc<GetTableSchemaRpc>(
      client,
      sync.AsStatusCallback(),
      table_id,
      info,
      deadline,
      include_hidden,
      resp);
  return sync.Wait();
}

Status YBClient::Data::GetTableSchema(YBClient* client,
                                      const YBTableName& table_name,
                                      CoarseTimePoint deadline,
                                      std::shared_ptr<YBTableInfo> info,
                                      StatusCallback callback,
                                      master::IncludeHidden include_hidden,
                                      master::GetTableSchemaResponsePB* resp_ignored) {
  auto rpc = StartRpc<GetTableSchemaRpc>(
      client,
      callback,
      table_name,
      info.get(),
      deadline,
      include_hidden);
  return Status::OK();
}

Status YBClient::Data::GetTableSchema(YBClient* client,
                                      TableIdView table_id,
                                      CoarseTimePoint deadline,
                                      std::shared_ptr<YBTableInfo> info,
                                      StatusCallback callback,
                                      master::IncludeHidden include_hidden,
                                      master::GetTableSchemaResponsePB* resp) {
  auto rpc = StartRpc<GetTableSchemaRpc>(
      client,
      callback,
      table_id,
      info.get(),
      deadline,
      include_hidden,
      resp);
  return Status::OK();
}

Status YBClient::Data::GetTablegroupSchemaById(
    YBClient* client,
    const TablegroupId& tablegroup_id,
    CoarseTimePoint deadline,
    std::shared_ptr<std::vector<YBTableInfo>> info,
    StatusCallback callback) {
  auto rpc = StartRpc<GetTablegroupSchemaRpc>(
      client,
      callback,
      tablegroup_id,
      info.get(),
      deadline);
  return Status::OK();
}

Result<IndexPermissions> YBClient::Data::GetIndexPermissions(
    YBClient* client,
    const TableId& table_id,
    const TableId& index_id,
    const CoarseTimePoint deadline) {
  YBTableInfo yb_table_info;

  RETURN_NOT_OK(GetTableSchema(client,
                               table_id,
                               deadline,
                               &yb_table_info));

  const auto* index_info = VERIFY_RESULT(yb_table_info.index_map.FindIndex(index_id));
  return index_info->index_permissions();
}

Result<IndexPermissions> YBClient::Data::GetIndexPermissions(
    YBClient* client,
    const YBTableName& table_name,
    const TableId& index_id,
    const CoarseTimePoint deadline) {
  YBTableInfo yb_table_info;

  RETURN_NOT_OK(GetTableSchema(client,
                               table_name,
                               deadline,
                               &yb_table_info));

  const auto* index_info = VERIFY_RESULT(yb_table_info.index_map.FindIndex(index_id));
  return index_info->index_permissions();
}

Result<IndexPermissions> YBClient::Data::WaitUntilIndexPermissionsAtLeast(
    YBClient* client,
    const TableId& table_id,
    const TableId& index_id,
    const IndexPermissions& target_index_permissions,
    const CoarseTimePoint deadline,
    const CoarseDuration max_wait) {
  const bool retry_on_not_found = (target_index_permissions != INDEX_PERM_NOT_USED);
  IndexPermissions actual_index_permissions = INDEX_PERM_NOT_USED;
  RETURN_NOT_OK(RetryUntilShutdown(
      deadline,
      "Waiting for index to have desired permissions",
      "Timed out waiting for proper index permissions",
      [&](CoarseTimePoint deadline, bool* retry) -> Status {
        Result<IndexPermissions> result = GetIndexPermissions(client, table_id, index_id, deadline);
        if (!result) {
          *retry = retry_on_not_found;
          return result.status();
        }
        actual_index_permissions = *result;
        *retry = actual_index_permissions < target_index_permissions;
        return Status::OK();
      },
      max_wait));
  // Now, the index permissions are guaranteed to be at (or beyond) the target.
  return actual_index_permissions;
}

Result<IndexPermissions> YBClient::Data::WaitUntilIndexPermissionsAtLeast(
    YBClient* client,
    const YBTableName& table_name,
    const YBTableName& index_name,
    const IndexPermissions& target_index_permissions,
    const CoarseTimePoint deadline,
    const CoarseDuration max_wait) {
  const bool retry_on_not_found = (target_index_permissions != INDEX_PERM_NOT_USED);
  IndexPermissions actual_index_permissions = INDEX_PERM_NOT_USED;
  YBTableInfo yb_index_info;
  RETURN_NOT_OK(RetryUntilShutdown(
      deadline,
      "Waiting for index table schema",
      "Timed out waiting for index table schema",
      [&](CoarseTimePoint deadline, bool* retry) -> Status {
        Status status = GetTableSchema(client,
                                     index_name,
                                     deadline,
                                     &yb_index_info);
        if (!status.ok()) {
          *retry = retry_on_not_found;
          return status;
        }
        *retry = false;
        return Status::OK();
      },
      max_wait));
  RETURN_NOT_OK(RetryUntilShutdown(
      deadline, "Waiting for index to have desired permissions",
      "Timed out waiting for proper index permissions",
      [&](CoarseTimePoint deadline, bool* retry) -> Status {
        Result<IndexPermissions> result = GetIndexPermissions(
            client,
            table_name,
            yb_index_info.table_id,
            deadline);
        if (!result) {
          *retry = retry_on_not_found;
          return result.status();
        }
        actual_index_permissions = *result;
        *retry = actual_index_permissions < target_index_permissions;
        return Status::OK();
      },
      max_wait));
  // Now, the index permissions are guaranteed to be at (or beyond) the target.
  return actual_index_permissions;
}

void YBClient::Data::CreateXClusterStream(
    YBClient* client, const TableId& table_id,
    const google::protobuf::RepeatedPtrField<yb::master::CDCStreamOptionsPB>& options,
    master::SysCDCStreamEntryPB::State state, cdc::StreamModeTransactional transactional,
    CoarseTimePoint deadline, CreateCDCStreamCallback callback) {
  auto rpc = StartRpc<internal::CreateXClusterStreamRpc>(
      client, callback, table_id, options, state, transactional, deadline);
}

void YBClient::Data::DeleteCDCStream(
    YBClient* client,
    const xrepl::StreamId& stream_id,
    CoarseTimePoint deadline,
    StatusCallback callback) {
  auto rpc = StartRpc<internal::DeleteCDCStreamRpc>(
      client, callback, stream_id, deadline);
}

Status YBClient::Data::BootstrapProducer(
    YBClient* client,
    const YQLDatabase& db_type,
    const NamespaceName& namespace_name,
    const std::vector<PgSchemaName>& pg_schema_names,
    const std::vector<TableName>& table_names,
    CoarseTimePoint deadline,
    BootstrapProducerCallback callback) {
  auto rpc =
      std::make_shared<internal::BootstrapProducerRpc>(client, std::move(callback), deadline);
  RETURN_NOT_OK(rpc->Init(db_type, namespace_name, pg_schema_names, table_names));
  rpcs_.RegisterAndStart(rpc, rpc->RpcHandle());

  return Status::OK();
}

void YBClient::Data::GetCDCDBStreamInfo(
    YBClient* client,
    const std::string& db_stream_id,
    std::shared_ptr<std::vector<pair<std::string, std::string>>> db_stream_info,
    CoarseTimePoint deadline,
    StdStatusCallback callback) {
  auto rpc = StartRpc<internal::GetCDCDBStreamInfoRpc>(
      client, callback, db_stream_id, db_stream_info.get(), deadline);
}

void YBClient::Data::GetCDCStream(
    YBClient* client,
    const xrepl::StreamId& stream_id,
    std::shared_ptr<ObjectId> object_id,
    std::shared_ptr<std::unordered_map<std::string, std::string>> options,
    CoarseTimePoint deadline,
    StdStatusCallback callback,
    std::shared_ptr<bool> xcluster_use_target_applied_filter) {
  auto rpc = StartRpc<internal::GetCDCStreamRpc>(
      client,
      callback,
      stream_id,
      object_id.get(),
      options.get(),
      deadline,
      xcluster_use_target_applied_filter.get());
}

void YBClient::Data::DeleteNotServingTablet(
    YBClient* client, const TabletId& tablet_id, CoarseTimePoint deadline,
    StdStatusCallback callback) {
  auto rpc = StartRpc<internal::DeleteNotServingTabletRpc>(
      client, tablet_id, callback, deadline);
}

void YBClient::Data::AcquireObjectLocksGlobalAsync(
    YBClient* client, master::AcquireObjectLocksGlobalRequestPB request, CoarseTimePoint deadline,
    StdStatusCallback callback, std::function<Status()>&& should_retry) {
  auto rpc = StartRpc<internal::AcquireObjectLocksGlobalRpc>(
      client, request, callback, deadline, std::move(should_retry));
}

void YBClient::Data::ReleaseObjectLocksGlobalAsync(
    YBClient* client, master::ReleaseObjectLocksGlobalRequestPB request, CoarseTimePoint deadline,
    StdStatusCallback callback) {
  auto rpc = StartRpc<internal::ReleaseObjectLocksGlobalRpc>(client, request, callback, deadline);
}

void YBClient::Data::WaitForLockersMultipleGlobalAsync(
    YBClient* client, master::WaitForLockersMultipleGlobalRequestPB request,
    CoarseTimePoint deadline, StdStatusCallback callback) {
  auto rpc = StartRpc<internal::WaitForLockersMultipleGlobalRpc>(
      client, request, callback, deadline);
}

void YBClient::Data::GetTableLocations(
    YBClient* client, const TableId& table_id, const int32_t max_tablets,
    const RequireTabletsRunning require_tablets_running, const PartitionsOnly partitions_only,
    const CoarseTimePoint deadline, GetTableLocationsCallback callback) {
  auto rpc = StartRpc<internal::GetTableLocationsRpc>(
      client, table_id, max_tablets, require_tablets_running, partitions_only,
      callback, deadline);
}

void YBClient::Data::LeaderMasterDetermined(const Status& status,
                                            const HostPort& host_port) {
  VLOG(4) << "YBClient: Leader master determined: status="
          << status.ToString() << ", host port ="
          << host_port.ToString();
  std::vector<StdStatusCallback> callbacks;
  {
    std::lock_guard l(leader_master_lock_);
    callbacks.swap(leader_master_callbacks_);

    if (status.ok()) {
      leader_master_hostport_ = host_port;
      master_admin_proxy_ = std::make_shared<master::MasterAdminProxy>(
          proxy_cache_.get(), host_port);
      master_backup_proxy_ =
          std::make_shared<master::MasterBackupProxy>(proxy_cache_.get(), host_port);
      master_client_proxy_ = std::make_shared<master::MasterClientProxy>(
          proxy_cache_.get(), host_port);
      master_cluster_proxy_ = std::make_shared<master::MasterClusterProxy>(
          proxy_cache_.get(), host_port);
      master_dcl_proxy_ = std::make_shared<master::MasterDclProxy>(
          proxy_cache_.get(), host_port);
      master_ddl_proxy_ = std::make_shared<master::MasterDdlProxy>(
          proxy_cache_.get(), host_port);
      master_replication_proxy_ = std::make_shared<master::MasterReplicationProxy>(
          proxy_cache_.get(), host_port);
      master_encryption_proxy_ = std::make_shared<master::MasterEncryptionProxy>(
          proxy_cache_.get(), host_port);
      master_test_proxy_ = std::make_shared<master::MasterTestProxy>(
          proxy_cache_.get(), host_port);
    }

    rpcs_.Unregister(&leader_master_rpc_);
  }

  for (const auto& callback : callbacks) {
    callback(status);
  }
}

Status YBClient::Data::SetMasterServerProxy(CoarseTimePoint deadline,
                                            bool skip_resolution,
                                            bool wait_for_leader_election) {
  Synchronizer sync;
  SetMasterServerProxyAsync(deadline, skip_resolution,
      wait_for_leader_election, sync.AsStdStatusCallback());
  return sync.Wait();
}

void YBClient::Data::SetMasterServerProxyAsync(CoarseTimePoint deadline,
                                               bool skip_resolution,
                                               bool wait_for_leader_election,
                                               const StdStatusCallback& callback)
    EXCLUDES(leader_master_lock_) {
  DCHECK(deadline != CoarseTimePoint::max());

  bool was_empty;
  {
    std::lock_guard l(leader_master_lock_);
    was_empty = leader_master_callbacks_.empty();
    leader_master_callbacks_.push_back(callback);
  }

  // It is the first callback, so we should trigger actual action.
  if (was_empty) {
    auto functor = std::bind(
        &Data::DoSetMasterServerProxy, this, deadline, skip_resolution, wait_for_leader_election);
    auto submit_status = threadpool_->SubmitFunc(functor);
    if (!submit_status.ok()) {
      LeaderMasterDetermined(submit_status, HostPort());
    }
  }
}

Result<server::MasterAddresses> YBClient::Data::ParseMasterAddresses(
    const Status& reinit_status) EXCLUDES(master_server_addrs_lock_) {
  server::MasterAddresses result;
  std::lock_guard l(master_server_addrs_lock_);
  if (!reinit_status.ok() && full_master_server_addrs_.empty()) {
    return reinit_status;
  }
  for (const std::string &master_server_addr : full_master_server_addrs_) {
    std::vector<HostPort> addrs;
    // TODO: Do address resolution asynchronously as well.
    RETURN_NOT_OK(HostPort::ParseStrings(master_server_addr, master::kMasterDefaultPort, &addrs));
    if (addrs.empty()) {
      return STATUS_FORMAT(
          InvalidArgument,
          "No master address specified by '$0' (all master server addresses: $1)",
          master_server_addr, full_master_server_addrs_);
    }

    result.push_back(std::move(addrs));
  }

  return result;
}

void YBClient::Data::DoSetMasterServerProxy(CoarseTimePoint deadline,
                                            bool skip_resolution,
                                            bool wait_for_leader_election) {
  // Refresh the value of 'master_server_addrs_' if needed.
  auto master_addrs = ParseMasterAddresses(ReinitializeMasterAddresses());

  if (!master_addrs.ok()) {
    LeaderMasterDetermined(master_addrs.status(), HostPort());
    return;
  }

  // Finding a new master involves a fan-out RPC to each master. A single
  // RPC timeout's worth of time should be sufficient, though we'll use
  // the provided deadline if it's sooner.
  auto leader_master_deadline = CoarseMonoClock::Now() + default_rpc_timeout_;
  auto actual_deadline = std::min(deadline, leader_master_deadline);

  if (skip_resolution && !master_addrs->empty() && !master_addrs->front().empty()) {
    LeaderMasterDetermined(Status::OK(), master_addrs->front().front());
    return;
  }

  ASH_ENABLE_CONCURRENT_UPDATES();
  SET_WAIT_STATUS(YBClient_WaitingOnMaster);
  rpcs_.Register(
      std::make_shared<GetLeaderMasterRpc>(
          Bind(&YBClient::Data::LeaderMasterDetermined, Unretained(this)),
          *master_addrs,
          actual_deadline,
          messenger_,
          proxy_cache_.get(),
          &rpcs_,
          false /*should timeout to follower*/,
          wait_for_leader_election),
      &leader_master_rpc_);
  (**leader_master_rpc_).SendRpc();
}

// API to clear and reset master addresses, used during master config change.
Status YBClient::Data::SetMasterAddresses(const string& addrs) {
  std::lock_guard l(master_server_addrs_lock_);
  if (addrs.empty()) {
    std::ostringstream out;
    out.str("Invalid empty master address cannot be set. Current list is: ");
    for (const string& master_server_addr : master_server_addrs_) {
      out.str(master_server_addr);
      out.str(" ");
    }
    LOG(DFATAL) << out.str();
    return STATUS(InvalidArgument, "master addresses cannot be empty");
  }

  master_server_addrs_.clear();
  master_server_addrs_.push_back(addrs);

  return Status::OK();
}

// Add a given master to the master address list.
Status YBClient::Data::AddMasterAddress(const HostPort& addr) {
  std::lock_guard l(master_server_addrs_lock_);
  master_server_addrs_.push_back(addr.ToString());
  return Status::OK();
}

Status YBClient::Data::CreateSnapshot(
    YBClient* client, const std::vector<YBTableName>& tables, CoarseTimePoint deadline,
    CreateSnapshotCallback callback) {
  auto rpc = std::make_shared<internal::CreateSnapshotRpc>(client, std::move(callback), deadline);
  RETURN_NOT_OK(rpc->Init(tables));
  rpcs_.RegisterAndStart(rpc, rpc->RpcHandle());

  return Status::OK();
}

namespace {

Result<std::string> ReadMasterAddressesFromFlagFile(
    const std::string& flag_file_path, const std::string& flag_name) {
  std::ifstream input_file(flag_file_path);
  if (!input_file) {
    return STATUS_FORMAT(IOError, "Unable to open flag file '$0': $1",
        flag_file_path, strerror(errno));
  }
  std::string line;

  std::string master_addrs;
  while (input_file.good() && std::getline(input_file, line)) {
    const std::string flag_prefix = "--" + flag_name + "=";
    if (boost::starts_with(line, flag_prefix)) {
      master_addrs = line.c_str() + flag_prefix.size();
    }
  }

  if (input_file.bad()) {
    // Do not check input_file.fail() here, reaching EOF may set that.
    return STATUS_FORMAT(IOError, "Failed reading flag file '$0': $1",
        flag_file_path, strerror(errno));
  }
  return master_addrs;
}

} // anonymous namespace

// Read the master addresses (from a remote endpoint or a file depending on which is specified), and
// re-initialize the 'master_server_addrs_' variable.
Status YBClient::Data::ReinitializeMasterAddresses() {
  Status result;
  std::lock_guard l(master_server_addrs_lock_);
  if (!FLAGS_flagfile.empty() && !skip_master_flagfile_) {
    LOG(INFO) << "Reinitialize master addresses from file: " << FLAGS_flagfile;
    auto master_addrs = ReadMasterAddressesFromFlagFile(
        FLAGS_flagfile, master_address_flag_name_);

    if (!master_addrs.ok()) {
      LOG(WARNING) << "Failure reading flagfile " << FLAGS_flagfile << ": "
                   << master_addrs.status();
      result = master_addrs.status();
    } else if (master_addrs->empty()) {
      LOG(WARNING) << "Couldn't find flag " << master_address_flag_name_ << " in flagfile "
                   << FLAGS_flagfile;
    } else {
      master_server_addrs_.clear();
      master_server_addrs_.push_back(*master_addrs);
    }
  } else {
    VLOG(1) << "Skipping reinitialize of master addresses, no REST endpoint or file specified";
  }
  full_master_server_addrs_.clear();
  for (const auto& address : master_server_addrs_) {
    if (!address.empty()) {
      full_master_server_addrs_.push_back(address);
    }
  }
  for (const auto& source : master_address_sources_) {
    auto current = source();
    full_master_server_addrs_.insert(
        full_master_server_addrs_.end(), current.begin(), current.end());
  }
  LOG(INFO) << "New master addresses: " << AsString(full_master_server_addrs_);

  if (full_master_server_addrs_.empty()) {
    return result.ok() ? STATUS(IllegalState, "Unable to determine master addresses") : result;
  }
  return Status::OK();
}

// Remove a given master from the list of master_server_addrs_.
Status YBClient::Data::RemoveMasterAddress(const HostPort& addr) {

  {
    auto str = addr.ToString();
    std::lock_guard l(master_server_addrs_lock_);
    auto it = std::ranges::find(master_server_addrs_, str);
    if (it != master_server_addrs_.end()) {
      master_server_addrs_.erase(it, it + str.size());
    }
  }

  return Status::OK();
}

Status YBClient::Data::SetReplicationInfo(
    YBClient* client, const ReplicationInfoPB& replication_info, CoarseTimePoint deadline,
    bool* retry) {
  // If retry was not set, we'll wrap around in a retryable function.
  if (!retry) {
    return RetryUntilShutdown(
        deadline, "Other clients changed the config. Retrying.",
        "Timed out retrying the config change. Probably too many concurrent attempts.",
        std::bind(&YBClient::Data::SetReplicationInfo, this, client, replication_info, _1, _2));
  }
  // Get the current config.
  GetMasterClusterConfigRequestPB get_req;
  GetMasterClusterConfigResponsePB get_resp;
  RETURN_NOT_OK(SyncLeaderMasterRpc(
      deadline, get_req, &get_resp, "GetMasterClusterConfig",
      &master::MasterClusterProxy::GetMasterClusterConfigAsync));

  ChangeMasterClusterConfigRequestPB change_req;
  ChangeMasterClusterConfigResponsePB change_resp;

  // Update the list with the new replication info.
  change_req.mutable_cluster_config()->CopyFrom(get_resp.cluster_config());
  auto new_ri = change_req.mutable_cluster_config()->mutable_replication_info();
  new_ri->CopyFrom(replication_info);

  // Try to update it on the live cluster.
  auto status = SyncLeaderMasterRpc(
      deadline, change_req, &change_resp, "ChangeMasterClusterConfig",
      &master::MasterClusterProxy::ChangeMasterClusterConfigAsync);
  if (change_resp.has_error()) {
    // Retry on config mismatch.
    *retry = change_resp.error().code() == MasterErrorPB::CONFIG_VERSION_MISMATCH;
  }
  RETURN_NOT_OK(status);
  *retry = false;
  return Status::OK();
}

Status YBClient::Data::ValidateReplicationInfo(
    const ReplicationInfoPB& replication_info, CoarseTimePoint deadline) {
  // Validate the request config.
  ValidateReplicationInfoRequestPB req;
  ValidateReplicationInfoResponsePB resp;
  auto new_ri = req.mutable_replication_info();
  new_ri->CopyFrom(replication_info);
  RETURN_NOT_OK(SyncLeaderMasterRpc(
      deadline, req, &resp, "ValidateReplicationInfo",
      &master::MasterReplicationProxy::ValidateReplicationInfoAsync));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  return Status::OK();
}

Result<TableSizeInfo> YBClient::Data::GetTableDiskSize(
    const TableId& table_id, CoarseTimePoint deadline) {
  master::GetTableDiskSizeRequestPB req;
  master::GetTableDiskSizeResponsePB resp;

  req.mutable_table()->set_table_id(table_id);

  RETURN_NOT_OK(SyncLeaderMasterRpc(
      deadline, req, &resp, "GetTableDiskSize",
      &master::MasterDdlProxy::GetTableDiskSizeAsync));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  return TableSizeInfo{
      .table_size = resp.size(), .num_missing_tablets = resp.num_missing_tablets()};
}

Status YBClient::Data::ReportYsqlDdlTxnStatus(
    const TransactionMetadata& txn, bool is_committed, const CoarseTimePoint& deadline) {
  master::ReportYsqlDdlTxnStatusRequestPB req;
  master::ReportYsqlDdlTxnStatusResponsePB resp;

  req.set_transaction_id(txn.transaction_id.data(), txn.transaction_id.size());
  req.set_is_committed(is_committed);
  RETURN_NOT_OK(SyncLeaderMasterRpc(
      deadline, req, &resp, "ReportYsqlDdlTxnStatus",
      &master::MasterDdlProxy::ReportYsqlDdlTxnStatusAsync));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }
  return Status::OK();
}

Status YBClient::Data::IsYsqlDdlVerificationInProgress(
    const TransactionMetadata& txn,
    CoarseTimePoint deadline,
    bool *ddl_verification_in_progress) {
  master::IsYsqlDdlVerificationDoneRequestPB req;
  master::IsYsqlDdlVerificationDoneResponsePB resp;

  txn.ToPB(req.mutable_transaction());
  auto s = SyncLeaderMasterRpc(
      deadline, req, &resp, "IsYsqlDdlVerificationDone",
      &master::MasterDdlProxy::IsYsqlDdlVerificationDoneAsync);

  // Check DDL verification is paused. This can happen during error injection tests.  If so, no
  // point continuing to retry.
  if (resp.has_error()) {
    if (resp.error().has_status() &&
        resp.error().status().has_message() &&
        resp.error().status().message().find("DDL Rollback is paused") != std::string::npos) {
      *ddl_verification_in_progress = false;
    }
    return StatusFromPB(resp.error().status());
  }

  RETURN_NOT_OK(s);

  *ddl_verification_in_progress = !resp.done();
  return Status::OK();
}

Status YBClient::Data::WaitForDdlVerificationToFinish(
    const TransactionMetadata& txn,
    CoarseTimePoint deadline) {
  return RetryUntilShutdown(
      deadline,
      Format("Waiting on YSQL DDL Verification for $0 to be completed", txn.transaction_id),
      Format("Timed out on YSQL DDL Verification for $0 to be completed", txn.transaction_id),
      std::bind(&YBClient::Data::IsYsqlDdlVerificationInProgress, this, txn, _1, _2));
}

Status YBClient::Data::RollbackDocdbSchemaToSubtxn(
  const TransactionMetadata& txn, SubTransactionId sub_txn_id, const CoarseTimePoint& deadline) {
  master::RollbackDocdbSchemaToSubtxnRequestPB req;
  master::RollbackDocdbSchemaToSubtxnResponsePB resp;

  req.set_transaction_id(txn.transaction_id.data(), txn.transaction_id.size());
  req.set_sub_transaction_id(sub_txn_id);
  RETURN_NOT_OK(SyncLeaderMasterRpc(
      deadline, req, &resp, "RollbackDocdbSchemaToSubtxn",
      &master::MasterDdlProxy::RollbackDocdbSchemaToSubtxnAsync));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }
  return Status::OK();
}

Status YBClient::Data::IsRollbackDocdbSchemaToSubtxnInProgress(
    const TransactionMetadata& txn, SubTransactionId sub_txn_id, CoarseTimePoint deadline,
    bool* rollback_to_sub_txn_in_progress) {
  master::IsRollbackDocdbSchemaToSubtxnDoneRequestPB req;
  master::IsRollbackDocdbSchemaToSubtxnDoneResponsePB resp;

  txn.ToPB(req.mutable_transaction());
  req.set_sub_transaction_id(sub_txn_id);
  auto status = SyncLeaderMasterRpc(
      deadline, req, &resp, "IsRollbackDocdbSchemaToSubtxnDone",
      &master::MasterDdlProxy::IsRollbackDocdbSchemaToSubtxnDoneAsync);

  *rollback_to_sub_txn_in_progress = !resp.done();
  return status;
}

Status YBClient::Data::WaitForRollbackDocdbSchemaToSubtxnToFinish(
    const TransactionMetadata& txn, SubTransactionId sub_txn_id, CoarseTimePoint deadline) {
  return RetryFunc(
      deadline,
      Format(
          "Waiting on YSQL Rollback to sub-transaction for $0-$1 to be completed",
          txn.transaction_id, sub_txn_id),
      Format(
          "Timed out on YSQL Rollback to sub-transaction for $0-$1 to be completed",
          txn.transaction_id, sub_txn_id),
      std::bind(
          &YBClient::Data::IsRollbackDocdbSchemaToSubtxnInProgress, this, txn, sub_txn_id, _1, _2));
}

Result<bool> YBClient::Data::CheckIfPitrActive(CoarseTimePoint deadline) {
  CheckIfPitrActiveRequestPB req;
  CheckIfPitrActiveResponsePB resp;

  Status status = SyncLeaderMasterRpc(
      deadline, req, &resp, "CheckIfPitrActive",
      &master::MasterAdminProxy::CheckIfPitrActiveAsync);
  RETURN_NOT_OK(status);

  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  return resp.is_pitr_active();
}

Status YBClient::Data::GetXClusterStreams(
    YBClient* client, CoarseTimePoint deadline,
    const xcluster::ReplicationGroupId& replication_group_id, const NamespaceId& namespace_id,
    const std::vector<TableName>& table_names, const std::vector<PgSchemaName>& pg_schema_names,
    std::function<void(Result<master::GetXClusterStreamsResponsePB>)> user_cb) {
  auto rpc =
      std::make_shared<internal::GetXClusterStreamsRpc>(client, std::move(user_cb), deadline);
  RETURN_NOT_OK(rpc->Init(replication_group_id, namespace_id, table_names, pg_schema_names));
  rpcs_.RegisterAndStart(rpc, rpc->RpcHandle());

  return Status::OK();
}

Status YBClient::Data::GetXClusterStreams(
    YBClient* client, CoarseTimePoint deadline,
    const xcluster::ReplicationGroupId& replication_group_id, const NamespaceId& namespace_id,
    const std::vector<TableId>& source_table_ids, bool create_stream_if_missing,
    std::function<void(Result<master::GetXClusterStreamsResponsePB>)> user_cb) {
  auto rpc =
      std::make_shared<internal::GetXClusterStreamsRpc>(client, std::move(user_cb), deadline);
  RETURN_NOT_OK(
      rpc->Init(replication_group_id, namespace_id, source_table_ids, create_stream_if_missing));
  rpcs_.RegisterAndStart(rpc, rpc->RpcHandle());

  return Status::OK();
}

Status YBClient::Data::IsXClusterBootstrapRequired(
    YBClient* client, CoarseTimePoint deadline,
    const xcluster::ReplicationGroupId& replication_group_id, const NamespaceId& namespace_id,
    std::function<void(Result<bool>)> user_cb) {
  auto rpc = std::make_shared<internal::IsXClusterBootstrapRequiredRpc>(
      client, std::move(user_cb), deadline);
  RETURN_NOT_OK(rpc->Init(replication_group_id, namespace_id));
  rpcs_.RegisterAndStart(rpc, rpc->RpcHandle());

  return Status::OK();
}

HostPort YBClient::Data::leader_master_hostport() const {
  std::lock_guard l(leader_master_lock_);
  return leader_master_hostport_;
}

shared_ptr<master::MasterAdminProxy> YBClient::Data::master_admin_proxy() const {
  std::lock_guard l(leader_master_lock_);
  return master_admin_proxy_;
}

shared_ptr<master::MasterBackupProxy> YBClient::Data::master_backup_proxy() const {
  std::lock_guard l(leader_master_lock_);
  return master_backup_proxy_;
}

shared_ptr<master::MasterClientProxy> YBClient::Data::master_client_proxy() const {
  std::lock_guard l(leader_master_lock_);
  return master_client_proxy_;
}

shared_ptr<master::MasterClusterProxy> YBClient::Data::master_cluster_proxy() const {
  std::lock_guard l(leader_master_lock_);
  return master_cluster_proxy_;
}

shared_ptr<master::MasterDclProxy> YBClient::Data::master_dcl_proxy() const {
  std::lock_guard l(leader_master_lock_);
  return master_dcl_proxy_;
}

shared_ptr<master::MasterDdlProxy> YBClient::Data::master_ddl_proxy() const {
  std::lock_guard l(leader_master_lock_);
  return master_ddl_proxy_;
}

shared_ptr<master::MasterReplicationProxy> YBClient::Data::master_replication_proxy() const {
  std::lock_guard l(leader_master_lock_);
  return master_replication_proxy_;
}

shared_ptr<master::MasterEncryptionProxy> YBClient::Data::master_encryption_proxy() const {
  std::lock_guard l(leader_master_lock_);
  return master_encryption_proxy_;
}

shared_ptr<master::MasterTestProxy> YBClient::Data::master_test_proxy() const {
  std::lock_guard l(leader_master_lock_);
  return master_test_proxy_;
}


uint64_t YBClient::Data::GetLatestObservedHybridTime() const {
  return latest_observed_hybrid_time_.Load();
}

void YBClient::Data::UpdateLatestObservedHybridTime(uint64_t hybrid_time) {
  latest_observed_hybrid_time_.StoreMax(hybrid_time);
}

void YBClient::Data::Shutdown() {
  closing_.store(true, std::memory_order_release);

  if (messenger_holder_) {
    messenger_holder_->Shutdown();
  }
  if (threadpool_) {
    {
      std::lock_guard lock(per_tag_tokens_mutex_);
      per_tag_tokens_.clear();
    }
    threadpool_->Shutdown();
    // Abort all in-progress rpcs using handle leader_master_rpc_.
    rpc::RpcCommandPtr rpc_to_abort = nullptr;
    {
      std::lock_guard l(leader_master_lock_);
      if (leader_master_rpc_ != rpcs_.InvalidHandle()) {
        rpc_to_abort = *leader_master_rpc_;
      }
    }
    if (rpc_to_abort) {
      rpc_to_abort->Abort();
    }
    // The tasks submitted to the threadpool_ in SetMasterServerProxyAsync would expect the
    // callbacks to be triggered at some point. Since threadpool_->Shutdown() destroys the
    // enqueued (but not currently running) tasks, invoke the callbacks inline.
    LeaderMasterDetermined(STATUS_FORMAT(ShutdownInProgress, "YBClient shutting down"), HostPort());
  }

  while (running_sync_requests_.load(std::memory_order_acquire)) {
    YB_LOG_EVERY_N_SECS(INFO, 5) << "Waiting sync requests to finish";
    std::this_thread::sleep_for(100ms);
  }
}

bool YBClient::Data::Closing() {
  return closing_.load();
}

bool YBClient::Data::IsMultiMaster() {
  std::lock_guard l(master_server_addrs_lock_);
  if (full_master_server_addrs_.size() > 1) {
    return true;
  }

  // For single entry case, first check if it is a list of hosts/ports.
  std::vector<HostPort> host_ports;
  auto status = HostPort::ParseStrings(full_master_server_addrs_[0],
                                       yb::master::kMasterDefaultPort,
                                       &host_ports);
  if (!status.ok()) {
    // Will fail ResolveAddresses as well, so log error and return false early.
    LOG(WARNING) << "Failure parsing address list: " << full_master_server_addrs_[0]
                 << ": " << status;
    return false;
  }
  if (host_ports.size() > 1) {
    return true;
  }

  // If we only have one HostPort, check if it resolves to multiple endpoints.
  std::vector<Endpoint> addrs;
  status = host_ports[0].ResolveAddresses(&addrs);
  return status.ok() && (addrs.size() > 1);
}

Status YBClient::Data::RetryUntilShutdown(
    CoarseTimePoint deadline, const std::string& retry_msg, const std::string& timeout_msg,
    const std::function<Status(CoarseTimePoint, bool*)>& func, const CoarseDuration max_wait,
    const uint32_t max_jitter_ms, const uint32_t init_exponent) {
  return RetryFunc(
      deadline, retry_msg, timeout_msg,
      [func, this](CoarseTimePoint deadline, bool* retry) {
        if (retry != nullptr && Closing()) {
          *retry = false;
          return STATUS(ShutdownInProgress, "Client is shutting down");
        }
        return func(deadline, retry);
      },
      max_wait, max_jitter_ms, init_exponent);
}

}  // namespace client
}  // namespace yb
