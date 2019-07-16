// Copyright (c) YugaByte, Inc.

#include "yb/rpc/secure_stream.h"

#include "yb/server/hybrid_clock.h"
#include "yb/server/secure.h"

#include "yb/tserver/tablet_server.h"
#include "yb/tserver/backup_service.h"

#include "yb/util/flag_tags.h"
#include "yb/util/ntp_clock.h"

DEFINE_int32(ts_backup_svc_num_threads, 4,
             "Number of RPC worker threads for the TS backup service");
TAG_FLAG(ts_backup_svc_num_threads, advanced);

DEFINE_int32(ts_backup_svc_queue_length, 50,
             "RPC queue length for the TS backup service");
TAG_FLAG(ts_backup_svc_queue_length, advanced);

namespace yb {
namespace tserver {
namespace enterprise {

using yb::rpc::ServiceIf;

TabletServer::TabletServer(const TabletServerOptions& opts) : super(opts) {
}

TabletServer::~TabletServer() {
}

Status TabletServer::RegisterServices() {
  server::HybridClock::RegisterProvider(NtpClock::Name(), [] {
    return std::make_shared<NtpClock>();
  });

  std::unique_ptr<ServiceIf> backup_service(
      new TabletServiceBackupImpl(tablet_manager_.get(), metric_entity()));
  RETURN_NOT_OK(RpcAndWebServerBase::RegisterService(FLAGS_ts_backup_svc_queue_length,
                                                     std::move(backup_service)));

  return super::RegisterServices();
}

Status TabletServer::SetupMessengerBuilder(rpc::MessengerBuilder* builder) {
  RETURN_NOT_OK(super::SetupMessengerBuilder(builder));
  secure_context_ = VERIFY_RESULT(
      server::SetupSecureContext(options_.rpc_opts.rpc_bind_addresses, fs_manager_.get(), builder));
  return Status::OK();
}

} // namespace enterprise
} // namespace tserver
} // namespace yb
