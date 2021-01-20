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

#include "yb/master/master.h"

#include "yb/master/master_backup.service.h"
#include "yb/master/master_backup_service.h"

#include "yb/rpc/secure_stream.h"

#include "yb/server/hybrid_clock.h"
#include "yb/server/secure.h"

#include "yb/util/flag_tags.h"
#include "yb/util/ntp_clock.h"

DEFINE_int32(master_backup_svc_queue_length, 50,
             "RPC queue length for master backup service");
TAG_FLAG(master_backup_svc_queue_length, advanced);

DECLARE_string(cert_node_filename);

namespace yb {
namespace master {
namespace enterprise {

using yb::rpc::ServiceIf;

Master::Master(const MasterOptions& opts) : super(opts) {
}

Master::~Master() {
}

Status Master::RegisterServices() {
#if !defined(__APPLE__)
  server::HybridClock::RegisterProvider(NtpClock::Name(), [](const std::string&) {
    return std::make_shared<NtpClock>();
  });
#endif

  std::unique_ptr<ServiceIf> master_backup_service(new MasterBackupServiceImpl(this));
  RETURN_NOT_OK(RpcAndWebServerBase::RegisterService(FLAGS_master_backup_svc_queue_length,
                                                     std::move(master_backup_service)));

  return super::RegisterServices();
}

Status Master::SetupMessengerBuilder(rpc::MessengerBuilder* builder) {
  RETURN_NOT_OK(super::SetupMessengerBuilder(builder));
  if (!FLAGS_cert_node_filename.empty()) {
    secure_context_ = VERIFY_RESULT(server::SetupSecureContext(
        server::DefaultRootDir(*fs_manager_),
        FLAGS_cert_node_filename,
        server::SecureContextType::kServerToServer,
        builder));
  } else {
    const string &hosts = !options_.server_broadcast_addresses.empty()
                        ? options_.server_broadcast_addresses
                        : options_.rpc_opts.rpc_bind_addresses;
    secure_context_ = VERIFY_RESULT(server::SetupSecureContext(
        hosts, *fs_manager_, server::SecureContextType::kServerToServer, builder));
  }

  return Status::OK();
}

} // namespace enterprise
} // namespace master
} // namespace yb
