//
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
//

#ifndef YB_TSERVER_SERVICE_UTIL_H
#define YB_TSERVER_SERVICE_UTIL_H

#include <boost/optional.hpp>

#include "yb/consensus/consensus_error.h"

#include "yb/rpc/rpc_context.h"
#include "yb/server/clock.h"

#include "yb/tablet/tablet_peer.h"

#include "yb/tserver/tablet_peer_lookup.h"
#include "yb/tablet/tablet_error.h"
#include "yb/tserver/tserver_error.h"

#include "yb/util/logging.h"

namespace yb {
namespace tserver {

// Non-template helpers.

void SetupErrorAndRespond(TabletServerErrorPB* error,
                          const Status& s,
                          TabletServerErrorPB::Code code,
                          rpc::RpcContext* context);

void SetupErrorAndRespond(TabletServerErrorPB* error,
                          const Status& s,
                          rpc::RpcContext* context);

Result<int64_t> LeaderTerm(const tablet::TabletPeer& tablet_peer);

// Template helpers.

template<class ReqClass, class RespClass>
bool CheckUuidMatchOrRespond(TabletPeerLookupIf* tablet_manager,
                             const char* method_name,
                             const ReqClass* req,
                             RespClass* resp,
                             rpc::RpcContext* context) {
  const string& local_uuid = tablet_manager->NodeInstance().permanent_uuid();
  if (req->dest_uuid().empty()) {
    // Maintain compat in release mode, but complain.
    string msg = strings::Substitute("$0: Missing destination UUID in request from $1: $2",
        method_name, context->requestor_string(), req->ShortDebugString());
#ifdef NDEBUG
    YB_LOG_EVERY_N(ERROR, 100) << msg;
#else
    LOG(FATAL) << msg;
#endif
    return true;
  }
  if (PREDICT_FALSE(req->dest_uuid() != local_uuid)) {
    const Status s = STATUS_SUBSTITUTE(InvalidArgument,
        "$0: Wrong destination UUID requested. Local UUID: $1. Requested UUID: $2",
        method_name, local_uuid, req->dest_uuid());
    LOG(WARNING) << s.ToString() << ": from " << context->requestor_string()
                 << ": " << req->ShortDebugString();
    SetupErrorAndRespond(resp->mutable_error(), s,
                         TabletServerErrorPB::WRONG_SERVER_UUID, context);
    return false;
  }
  return true;
}

template <class RespType>
void HandleErrorResponse(RespType* resp, rpc::RpcContext* context, const Status& s,
    const boost::optional<TabletServerErrorPB::Code>& error_code = boost::none) {
  resp->Clear();
  SetupErrorAndRespond(resp->mutable_error(), s,
      error_code.get_value_or(TabletServerErrorPB::UNKNOWN_ERROR), context);
}

template <class RespType>
void HandleResponse(RespType* resp,
                    const std::shared_ptr<rpc::RpcContext>& context,
                    const Status& s) {
  if (PREDICT_FALSE(!s.ok())) {
    HandleErrorResponse(resp, context.get(), s);
    return;
  }
  context->RespondSuccess();
}

template <class RespType>
StdStatusCallback BindHandleResponse(RespType* resp,
                                  const std::shared_ptr<rpc::RpcContext>& context) {
  return std::bind(&HandleResponse<RespType>, resp, context, std::placeholders::_1);
}

struct TabletPeerTablet {
  std::shared_ptr<tablet::TabletPeer> tablet_peer;
  tablet::TabletPtr tablet;
};

// Lookup the given tablet, ensuring that it both exists and is RUNNING.
// If it is not, respond to the RPC associated with 'context' after setting
// resp->mutable_error() to indicate the failure reason.
//
// Returns true if successful.
template<class RespClass>
Result<TabletPeerTablet> LookupTabletPeerOrRespond(
    TabletPeerLookupIf* tablet_manager,
    const string& tablet_id,
    RespClass* resp,
    rpc::RpcContext* context) {
  TabletPeerTablet result;
  Status status = tablet_manager->GetTabletPeer(tablet_id, &result.tablet_peer);
  if (PREDICT_FALSE(!status.ok())) {
    TabletServerErrorPB::Code code = status.IsServiceUnavailable() ?
                                     TabletServerErrorPB::UNKNOWN_ERROR :
                                     TabletServerErrorPB::TABLET_NOT_FOUND;
    SetupErrorAndRespond(resp->mutable_error(), status, code, context);
    return status;
  }

  // Check RUNNING state.
  tablet::RaftGroupStatePB state = result.tablet_peer->state();
  if (PREDICT_FALSE(state != tablet::RUNNING)) {
    Status s = STATUS(IllegalState, "Tablet not RUNNING", tablet::RaftGroupStateError(state))
        .CloneAndAddErrorCode(TabletServerError(TabletServerErrorPB::TABLET_NOT_RUNNING));
    SetupErrorAndRespond(resp->mutable_error(), s, context);
    return s;
  }

  result.tablet = result.tablet_peer->shared_tablet();
  if (!result.tablet) {
    Status s = STATUS(IllegalState,
                      "Tablet not running",
                      TabletServerError(TabletServerErrorPB::TABLET_NOT_RUNNING));
    SetupErrorAndRespond(resp->mutable_error(), s, context);
    return s;
  }
  return result;
}

// A transaction completion callback that responds to the client when transactions
// complete and sets the client error if there is one to set.
template<class Response>
class RpcOperationCompletionCallback : public tablet::OperationCompletionCallback {
 public:
  RpcOperationCompletionCallback(
      rpc::RpcContext context,
      Response* const response,
      const server::ClockPtr& clock)
      : context_(std::move(context)), response_(response), clock_(clock) {}

  void OperationCompleted() override {
    bool expected = false;
    if (!responded_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return;
    }
    if (clock_) {
      response_->set_propagated_hybrid_time(clock_->Now().ToUint64());
    }
    if (!status_.ok()) {
      SetupErrorAndRespond(get_error(), status_, code_, &context_);
    } else {
      context_.RespondSuccess();
    }
  }

 private:

  TabletServerErrorPB* get_error() {
    return response_->mutable_error();
  }

  rpc::RpcContext context_;
  Response* const response_;
  server::ClockPtr clock_;
  std::atomic<bool> responded_{false};
};

template<class Response>
std::unique_ptr<tablet::OperationCompletionCallback> MakeRpcOperationCompletionCallback(
    rpc::RpcContext context,
    Response* response,
    const server::ClockPtr& clock) {
  return std::make_unique<RpcOperationCompletionCallback<Response>>(
      std::move(context), response, clock);
}

struct LeaderTabletPeer {
  tablet::TabletPeerPtr peer;
  tablet::TabletPtr tablet;
  int64_t leader_term;

  bool operator!() const {
    return !peer;
  }

  bool FillTerm(TabletServerErrorPB* error, rpc::RpcContext* context);
  void FillTabletPeer(TabletPeerTablet source);
};

// The "peer" argument could be provided by the caller in case the caller has already performed
// the LookupTabletPeerOrRespond call, and we only need to fill the leader term.
template<class RespClass>
LeaderTabletPeer LookupLeaderTabletOrRespond(
    TabletPeerLookupIf* tablet_manager,
    const std::string& tablet_id,
    RespClass* resp,
    rpc::RpcContext* context,
    TabletPeerTablet peer = TabletPeerTablet()) {
  if (peer.tablet_peer) {
    LOG_IF(DFATAL, peer.tablet_peer->tablet_id() != tablet_id)
        << "Mismatching table ids: peer " << peer.tablet_peer->tablet_id()
        << " vs " << tablet_id;
    LOG_IF(DFATAL, !peer.tablet) << "Empty tablet pointer for tablet id : " << tablet_id;
  } else {
    auto peer_result = LookupTabletPeerOrRespond(tablet_manager, tablet_id, resp, context);
    if (!peer_result.ok()) {
      return LeaderTabletPeer();
    }
    peer = std::move(*peer_result);
  }
  LeaderTabletPeer result;
  result.FillTabletPeer(std::move(peer));

  if (!result.FillTerm(resp->mutable_error(), context)) {
    return LeaderTabletPeer();
  }
  resp->clear_error();

  return result;
}

}  // namespace tserver
}  // namespace yb

// Macro helpers.

#define RETURN_UNKNOWN_ERROR_IF_NOT_OK(s, resp, context)       \
  do {                                                         \
    Status ss = s;                                             \
    if (PREDICT_FALSE(!ss.ok())) {                             \
      SetupErrorAndRespond((resp)->mutable_error(), ss,        \
                           TabletServerErrorPB::UNKNOWN_ERROR, \
                           (context));                         \
      return;                                                  \
    }                                                          \
  } while (0)

#endif // YB_TSERVER_SERVICE_UTIL_H
