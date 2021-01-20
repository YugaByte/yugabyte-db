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

#include "yb/client/transaction_rpc.h"

#include <boost/preprocessor/cat.hpp>

#include "yb/client/client.h"
#include "yb/client/meta_cache.h"
#include "yb/client/tablet_rpc.h"

#include "yb/rpc/rpc.h"

#include "yb/tserver/tserver_service.pb.h"
#include "yb/tserver/tserver_service.proxy.h"

using namespace std::literals;

namespace yb {
namespace client {

namespace {

class TransactionRpcBase : public rpc::Rpc, public internal::TabletRpc {
 public:
  TransactionRpcBase(CoarseTimePoint deadline,
                     internal::RemoteTablet* tablet,
                     YBClient* client)
      : rpc::Rpc(deadline, client->messenger(), &client->proxy_cache()),
        trace_(new Trace),
        invoker_(false /* local_tserver_only */,
                 false /* consistent_prefix */,
                 client,
                 this,
                 this,
                 tablet,
                 /* table =*/ nullptr,
                 mutable_retrier(),
                 trace_.get()) {
  }

  virtual ~TransactionRpcBase() {}

  void SendRpc() override {
    invoker_.Execute(tablet_id());
  }

  void Finished(const Status& status) override {
    Status new_status = status;
    if (invoker_.Done(&new_status)) {
      auto retain_self = shared_from_this();
      InvokeCallback(new_status);
    }
  }

  // TODO(dtxn)
  void Failed(const Status& status) override {}

  void Abort() override {
    rpc::Rpc::Abort();
  }

 private:
  void SendRpcToTserver(int attempt_num) override {
    InvokeAsync(invoker_.proxy().get(),
                PrepareController(),
                std::bind(&TransactionRpcBase::Finished, this, Status::OK()));
  }

  virtual void InvokeCallback(const Status& status) = 0;
  virtual const TabletId& tablet_id() const = 0;
  virtual void InvokeAsync(tserver::TabletServerServiceProxy* proxy,
                           rpc::RpcController* controller,
                           rpc::ResponseCallback callback) = 0;

  TracePtr trace_;
  internal::TabletInvoker invoker_;
};

// UpdateTransactionRpc is used to call UpdateTransaction remote method of appropriate tablet.
template <class Traits>
class TransactionRpc : public TransactionRpcBase {
 public:
  TransactionRpc(CoarseTimePoint deadline,
                 internal::RemoteTablet* tablet,
                 YBClient* client,
                 typename Traits::Request* req,
                 typename Traits::Callback callback)
      : TransactionRpcBase(deadline, tablet, client),
        callback_(std::move(callback)) {
    req_.Swap(req);
  }

  virtual ~TransactionRpc() {}

  const tserver::TabletServerErrorPB* response_error() const override {
    return resp_.has_error() ? &resp_.error() : nullptr;
  }

 private:
  const std::string& tablet_id() const override {
    return req_.tablet_id();
  }

  std::string ToString() const override {
    return Format("$0: $1, retrier: $2", Traits::kName, req_, retrier());
  }

  void InvokeCallback(const Status& status) override {
    Traits::CallCallback(callback_, status, resp_);
  }

  void InvokeAsync(tserver::TabletServerServiceProxy* proxy,
                   rpc::RpcController* controller,
                   rpc::ResponseCallback callback) override {
    Traits::InvokeAsync(proxy, req_, &resp_, controller, std::move(callback));
  }

  typename Traits::Request req_;
  typename Traits::Response resp_;
  typename Traits::Callback callback_;
};

#define TRANSACTION_RPC_TRAITS(i, data, entry) \
struct BOOST_PP_CAT(entry, Traits) { \
  static constexpr const char* kName = BOOST_PP_STRINGIZE(entry); \
  typedef TRANSACTION_RPC_REQUEST_PB(entry) Request; \
  typedef TRANSACTION_RPC_RESPONSE_PB(entry) Response; \
  typedef TRANSACTION_RPC_CALLBACK(entry) Callback; \
  \
  static void CallCallback( \
      const Callback& callback, const Status& status, const Response& response) { \
    callback(status, response); \
  } \
  \
  static void InvokeAsync(tserver::TabletServerServiceProxy* proxy, \
                          const Request& request, \
                          Response* response, \
                          rpc::RpcController* controller, \
                          rpc::ResponseCallback callback) { \
    proxy->BOOST_PP_CAT(entry, Async)(request, response, controller, std::move(callback)); \
  } \
}; \
\
constexpr const char* BOOST_PP_CAT(entry, Traits)::kName;

BOOST_PP_SEQ_FOR_EACH(TRANSACTION_RPC_TRAITS, ~, TRANSACTION_RPCS)

} // namespace

#define TRANSACTION_RPC_BODY(entry) { \
  return std::make_shared<TransactionRpc<BOOST_PP_CAT(entry, Traits)>>( \
      deadline, tablet, client, req, std::move(callback)); \
  }

BOOST_PP_SEQ_FOR_EACH(TRANSACTION_RPC_FUNCTION, TRANSACTION_RPC_BODY, TRANSACTION_RPCS)

} // namespace client
} // namespace yb
