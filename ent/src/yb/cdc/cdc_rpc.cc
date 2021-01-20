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

#include "yb/cdc/cdc_rpc.h"

#include "yb/cdc/cdc_service.pb.h"
#include "yb/cdc/cdc_service.proxy.h"

#include "yb/client/client.h"
#include "yb/client/client-internal.h"
#include "yb/client/meta_cache.h"
#include "yb/client/tablet_rpc.h"

#include "yb/rpc/rpc.h"

#include "yb/tserver/tserver_service.pb.h"
#include "yb/tserver/tserver_service.proxy.h"
#include "yb/tserver/tserver.pb.h"


using namespace std::literals;

using yb::tserver::TabletServerErrorPB;
using yb::tserver::TabletServerServiceProxy;
using yb::tserver::WriteRequestPB;
using yb::tserver::WriteResponsePB;

namespace yb {
namespace cdc {

class CDCWriteRpc : public rpc::Rpc, public client::internal::TabletRpc {
 public:
  CDCWriteRpc(CoarseTimePoint deadline,
              client::internal::RemoteTablet *tablet,
              client::YBClient *client,
              WriteRequestPB *req,
              WriteCDCRecordCallback callback,
              bool use_local_tserver)
      : rpc::Rpc(deadline, client->messenger(), &client->proxy_cache()),
        trace_(new Trace),
        invoker_(use_local_tserver /* local_tserver_only */,
                 false /* consistent_prefix */,
                 client,
                 this,
                 this,
                 tablet,
                 // TODO(tsplit): decide whether we need to get info about stale table partitions
                 // here.
                 /* table =*/ nullptr,
                 mutable_retrier(),
                 trace_.get()),
        callback_(std::move(callback)) {
    req_.Swap(req);
  }

  virtual ~CDCWriteRpc() {
    CHECK(called_);
  }

  void SendRpc() override {
    invoker_.Execute(tablet_id());
  }

  void Finished(const Status &status) override {
    Status new_status = status;
    if (invoker_.Done(&new_status)) {
      InvokeCallback(new_status);
    }
  }

  void Failed(const Status &status) override {}

  void Abort() override {
    rpc::Rpc::Abort();
  }

  const TabletServerErrorPB *response_error() const override {
    return resp_.has_error() ? &resp_.error() : nullptr;
  }

 private:
  void SendRpcToTserver(int attempt_num) override {
    InvokeAsync(invoker_.proxy().get(),
                PrepareController(),
                std::bind(&CDCWriteRpc::Finished, this, Status::OK()));
  }

  const std::string &tablet_id() const {
    return req_.tablet_id();
  }

  std::string ToString() const override {
    return Format("CDCWriteRpc: $0, retrier: $1", req_, retrier());
  }

  void InvokeCallback(const Status &status) {
    if (!called_) {
      called_ = true;
      callback_(status, resp_);
    } else {
      LOG(WARNING) << "Multiple invocation of CDCWriteRpc: "
                   << status.ToString() << " : " << resp_.DebugString();
    }
  }

  void InvokeAsync(TabletServerServiceProxy *proxy,
                   rpc::RpcController *controller,
                   rpc::ResponseCallback callback) {
    proxy->WriteAsync(req_, &resp_, controller, std::move(callback));
  }

  TracePtr trace_;
  client::internal::TabletInvoker invoker_;
  WriteRequestPB req_;
  WriteResponsePB resp_;
  WriteCDCRecordCallback callback_;
  bool called_ = false;
};

rpc::RpcCommandPtr CreateCDCWriteRpc(
    CoarseTimePoint deadline,
    client::internal::RemoteTablet* tablet,
    client::YBClient* client,
    WriteRequestPB* req,
    WriteCDCRecordCallback callback,
    bool use_local_tserver) {
  return std::make_shared<CDCWriteRpc>(
      deadline, tablet, client, req, std::move(callback), use_local_tserver);
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

class CDCReadRpc : public rpc::Rpc, public client::internal::TabletRpc {
 public:
  CDCReadRpc(CoarseTimePoint deadline,
             client::internal::RemoteTablet *tablet,
             client::YBClient *client,
             GetChangesRequestPB* req,
             GetChangesCDCRpcCallback callback)
      : rpc::Rpc(deadline, client->messenger(), &client->proxy_cache()),
        trace_(new Trace),
        invoker_(false /* local_tserver_only */,
                 false /* consistent_prefix */,
                 client,
                 this,
                 this,
                 tablet,
                 // TODO(tsplit): decide whether we need to get info about stale table partitions
                 // here.
                 /* table =*/ nullptr,
                 mutable_retrier(),
                 trace_.get()),
        callback_(std::move(callback)) {
    req_.Swap(req);
  }

  virtual ~CDCReadRpc() {
    CHECK(called_);
  }

  void SendRpc() override {
    invoker_.Execute(tablet_id());
  }

  void Finished(const Status &status) override {
    auto retained = shared_from_this(); // Ensure we don't destruct until after the callback.
    Status new_status = status;
    if (invoker_.Done(&new_status)) {
      InvokeCallback(new_status);
    }
  }

  void Failed(const Status &status) override { }

  void Abort() override {
    rpc::Rpc::Abort();
  }

  const tserver::TabletServerErrorPB *response_error() const override {
    // Clear the contents of last_error_, since this function is invoked again on retry.
    last_error_.Clear();

    if (resp_.has_error()) {
      if (resp_.error().has_code()) {
        // Map CDC Errors to TabletServer Errors.
        switch (resp_.error().code()) {
          case CDCErrorPB::TABLET_NOT_FOUND:
            last_error_.set_code(tserver::TabletServerErrorPB::TABLET_NOT_FOUND);
            if (resp_.error().has_status()) {
              last_error_.mutable_status()->CopyFrom(resp_.error().status());
            }
            return &last_error_;
          case CDCErrorPB::LEADER_NOT_READY:
            last_error_.set_code(tserver::TabletServerErrorPB::LEADER_NOT_READY_TO_SERVE);
            if (resp_.error().has_status()) {
              last_error_.mutable_status()->CopyFrom(resp_.error().status());
            }
            return &last_error_;
          // TS.STALE_FOLLOWER => pattern not used.
          default:
            break;
        }
      }
    }
    return nullptr;
  }

  void SendRpcToTserver(int attempt_num) override {
    // should be fast because the proxy cache has EndPoint from the tablet lookup.
    cdc_proxy_ = std::make_shared<CDCServiceProxy>(
       &invoker_.client().proxy_cache(), invoker_.ProxyEndpoint());

    auto self = std::static_pointer_cast<CDCReadRpc>(shared_from_this());
    InvokeAsync(cdc_proxy_.get(),
        PrepareController(),
        std::bind(&CDCReadRpc::Finished, self, Status::OK()));
  }

 private:
  const std::string &tablet_id() const {
    return req_.tablet_id();
  }

  std::string ToString() const override {
    return Format("CDCReadRpc: $0, retrier: $1", req_, retrier());
  }

  void InvokeCallback(const Status &status) {
    if (!called_) {
      called_ = true;
      callback_(status, std::move(resp_));
    } else {
      LOG(WARNING) << "Multiple invocation of CDCReadRpc: "
                   << status.ToString() << " : " << resp_.DebugString();
    }
  }

  void InvokeAsync(CDCServiceProxy *cdc_proxy,
                   rpc::RpcController *controller,
                   rpc::ResponseCallback callback) {
    cdc_proxy->GetChangesAsync(req_, &resp_, controller, std::move(callback));
  }

  TracePtr trace_;
  client::internal::TabletInvoker invoker_;

  GetChangesRequestPB req_;
  GetChangesResponsePB resp_;
  GetChangesCDCRpcCallback callback_;

  std::shared_ptr<CDCServiceProxy> cdc_proxy_;
  mutable tserver::TabletServerErrorPB last_error_;
  bool called_ = false;
};

MUST_USE_RESULT rpc::RpcCommandPtr CreateGetChangesCDCRpc(
    CoarseTimePoint deadline,
    client::internal::RemoteTablet* tablet,
    client::YBClient* client,
    GetChangesRequestPB* req,
    GetChangesCDCRpcCallback callback) {
  return std::make_shared<CDCReadRpc>(
      deadline, tablet, client, req, std::move(callback));
}


} // namespace cdc
} // namespace yb
