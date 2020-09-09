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

#include "yb/tserver/tablet_server_options.h"

#include "yb/master/master.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/tserver_flags.h"

using std::vector;

namespace yb {
namespace tserver {

const char* TabletServerOptions::kServerType = "tserver";

Result<TabletServerOptions> TabletServerOptions::CreateTabletServerOptions() {
  server::MasterAddresses master_addresses;
  std::string master_addresses_resolved_str;
  RETURN_NOT_OK(server::DetermineMasterAddresses(
      "tserver_master_addrs", FLAGS_tserver_master_addrs,
      FLAGS_tserver_master_replication_factor, &master_addresses, &master_addresses_resolved_str));

  TabletServerOptions opts(std::make_shared<server::MasterAddresses>(std::move(master_addresses)));
  opts.master_addresses_flag = master_addresses_resolved_str;
  return opts;
}

TabletServerOptions::TabletServerOptions(server::MasterAddressesPtr master_addresses)
    : ServerBaseOptions(TabletServer::kDefaultPort) {
  server_type = kServerType;

  SetMasterAddresses(std::move(master_addresses));
  ValidateMasterAddresses();
}


void TabletServerOptions::ValidateMasterAddresses() const {
  auto master_addresses = GetMasterAddresses();
  if (master_addresses->empty()) {
    LOG(FATAL) << "No masters were specified in the master addresses flag '"
               << master_addresses_flag << "', but a minimum of one is required.";
  }
}

}  // namespace tserver
}  // namespace yb
