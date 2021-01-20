// Copyright (c) YugaByte, Inc.
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
#include "yb/master/master_tablet_service.h"

#include "yb/master/catalog_manager-internal.h"

#include "yb/util/flag_tags.h"
#include "yb/util/random_util.h"

DEFINE_test_flag(int32, ysql_catalog_write_rejection_percentage, 0,
                 "Reject specified percentage of writes to the YSQL catalog tables.");
TAG_FLAG(TEST_ysql_catalog_write_rejection_percentage, runtime);

namespace yb {
namespace master {

// Only SysTablet 0 is bootstrapped on all master peers, and only the master leader
// reads other sys tablets. We check only for tablet 0 so as to have same readiness
// level across all masters.
// Note: If this value changes, then IsTabletServerReady has to be revisited.
constexpr int NUM_TABLETS_SYS_CATALOG = 1;

MasterTabletServiceImpl::MasterTabletServiceImpl(MasterTabletServer* server, Master* master)
    : TabletServiceImpl(server), master_(master) {
}

bool MasterTabletServiceImpl::GetTabletOrRespond(
    const tserver::ReadRequestPB* req,
    tserver::ReadResponsePB* resp,
    rpc::RpcContext* context,
    std::shared_ptr<tablet::AbstractTablet>* tablet,
    tablet::TabletPeerPtr looked_up_tablet_peer) {
  // Ignore looked_up_tablet_peer.

  CatalogManager::ScopedLeaderSharedLock l(master_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespondTServer(resp, context)) {
    return false;
  }

  const auto result = master_->catalog_manager()->GetSystemTablet(req->tablet_id());
  if (PREDICT_FALSE(!result)) {
    tserver::TabletServerErrorPB* error = resp->mutable_error();
    StatusToPB(result.status(), error->mutable_status());
    error->set_code(tserver::TabletServerErrorPB::TABLET_NOT_FOUND);
    context->RespondSuccess();
    return false;
  }

  *tablet = *result;
  return true;
}

void MasterTabletServiceImpl::Write(const tserver::WriteRequestPB* req,
                                    tserver::WriteResponsePB* resp,
                                    rpc::RpcContext context) {
  CatalogManager::ScopedLeaderSharedLock l(master_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespondTServer(resp, &context)) {
    return;
  }

  if (PREDICT_FALSE(FLAGS_TEST_ysql_catalog_write_rejection_percentage > 0) &&
      req->pgsql_write_batch_size() > 0 &&
      RandomUniformInt(1, 99) <= FLAGS_TEST_ysql_catalog_write_rejection_percentage) {
    context.RespondRpcFailure(rpc::ErrorStatusPB::ERROR_APPLICATION,
        STATUS(InternalError, "Injected random failure for testing."));
      return;
  }

  for (const auto& pg_req : req->pgsql_write_batch()) {
    if (pg_req.is_ysql_catalog_change()) {
      const auto &res = master_->catalog_manager()->IncrementYsqlCatalogVersion();
      if (!res.ok()) {
        context.RespondRpcFailure(rpc::ErrorStatusPB::ERROR_APPLICATION,
            STATUS(InternalError, "Failed to increment YSQL catalog version"));
      }
    }
  }

  tserver::TabletServiceImpl::Write(req, resp, std::move(context));
}

void MasterTabletServiceImpl::IsTabletServerReady(
    const tserver::IsTabletServerReadyRequestPB* req,
    tserver::IsTabletServerReadyResponsePB* resp,
    rpc::RpcContext context) {
  CatalogManager::ScopedLeaderSharedLock l(master_->catalog_manager());
  int total_tablets = NUM_TABLETS_SYS_CATALOG;
  resp->set_total_tablets(total_tablets);
  resp->set_num_tablets_not_running(total_tablets);

  // Tablet 0 being ready corresponds to state_ = kRunning in catalog manager.
  // If catalog_status_ in not OK, then catalog manager state_ is not kRunning.
  if (!l.CheckIsInitializedOrRespondTServer(resp, &context, false /* set_error */)) {
    LOG(INFO) << "Zero tablets not running out of " << total_tablets;
  } else {
    LOG(INFO) << "All " << total_tablets << " tablets running.";
    resp->set_num_tablets_not_running(0);
    context.RespondSuccess();
  }
}

namespace {

void HandleUnsupportedMethod(const char* method_name, rpc::RpcContext* context) {
  context->RespondRpcFailure(rpc::ErrorStatusPB::ERROR_APPLICATION,
                             STATUS_FORMAT(NotSupported, "$0 Not Supported!", method_name));
}

} // namespace

void MasterTabletServiceImpl::ListTablets(const tserver::ListTabletsRequestPB* req,
                                          tserver::ListTabletsResponsePB* resp,
                                          rpc::RpcContext context)  {
  HandleUnsupportedMethod("ListTablets", &context);
}

void MasterTabletServiceImpl::ListTabletsForTabletServer(
    const tserver::ListTabletsForTabletServerRequestPB* req,
    tserver::ListTabletsForTabletServerResponsePB* resp,
    rpc::RpcContext context)  {
  HandleUnsupportedMethod("ListTabletsForTabletServer", &context);
}

void MasterTabletServiceImpl::GetLogLocation(const tserver::GetLogLocationRequestPB* req,
                                             tserver::GetLogLocationResponsePB* resp,
                                             rpc::RpcContext context)  {
  HandleUnsupportedMethod("GetLogLocation", &context);
}

void MasterTabletServiceImpl::Checksum(const tserver::ChecksumRequestPB* req,
                                       tserver::ChecksumResponsePB* resp,
                                       rpc::RpcContext context)  {
  HandleUnsupportedMethod("Checksum", &context);
}

} // namespace master
} // namespace yb
