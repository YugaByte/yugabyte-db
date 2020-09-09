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
#ifndef YB_SERVER_SERVER_BASE_OPTIONS_H
#define YB_SERVER_SERVER_BASE_OPTIONS_H

#include <string>
#include <vector>
#include <mutex>

#include "yb/common/common.pb.h"
#include "yb/fs/fs_manager.h"
#include "yb/server/webserver_options.h"
#include "yb/server/rpc_server.h"
#include "yb/util/net/net_util.h"

namespace yb {

class Env;

namespace server {

typedef std::vector<std::vector<HostPort>> MasterAddresses;
typedef std::shared_ptr<const MasterAddresses> MasterAddressesPtr;

// Options common to both types of servers.
// The subclass constructor should fill these in with defaults from
// server-specific flags.
class ServerBaseOptions {
 public:
  Env* env;

  // This field is to be used as a path component for all the fs roots by FsManager. For now, we
  // expect it to be either master or tserver and passed in from the respective Server object.
  std::string server_type;

  FsManagerOpts fs_opts;
  RpcServerOptions rpc_opts;
  WebserverOptions webserver_opts;

  std::string dump_info_path;
  std::string dump_info_format;

  int32_t metrics_log_interval_ms;

  const std::string& placement_cloud() const;
  const std::string& placement_region() const;
  const std::string& placement_zone() const;

  bool has_placement_cloud() const;
  void SetPlacement(std::string cloud, std::string region, std::string zone);

  std::string placement_uuid;

  std::string master_addresses_flag;

  std::vector<HostPort> broadcast_addresses;

  // This can crash the process if you pass in an invalid list of master addresses!
  void SetMasterAddresses(MasterAddressesPtr master_addresses) {
    CHECK_NOTNULL(master_addresses.get());

    SetMasterAddressesNoValidation(std::move(master_addresses));
  }

  MasterAddressesPtr GetMasterAddresses() const;

  CloudInfoPB MakeCloudInfoPB() const;

  ServerBaseOptions(const ServerBaseOptions& options);

 protected:
  explicit ServerBaseOptions(int default_port);

 private:
  void SetMasterAddressesNoValidation(MasterAddressesPtr master_addresses);

  // List of masters this server is aware of. This will get recreated on a master config change.
  // We should ensure that the vector elements are not individually updated. And the shared pointer
  // will guarantee inconsistent in-transit views of the vector are never seen during/across
  // config changes.
  MasterAddressesPtr master_addresses_;

  std::string placement_cloud_;
  std::string placement_region_;
  std::string placement_zone_;

  // Mutex to avoid concurrent access to the variable above.
  mutable std::mutex master_addresses_mtx_;
};

CHECKED_STATUS DetermineMasterAddresses(
    const std::string& master_addresses_flag_name, const std::string& master_addresses_flag,
    uint64_t master_replication_factor, MasterAddresses* master_addresses,
    std::string* master_addresses_resolved_str);

std::string MasterAddressesToString(const MasterAddresses& addresses);

CHECKED_STATUS ResolveMasterAddresses(
    MasterAddressesPtr master_addresses, std::vector<Endpoint>* resolved_addresses);

} // namespace server
} // namespace yb

#endif /* YB_SERVER_SERVER_BASE_OPTIONS_H */
