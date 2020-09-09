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

#include "yb/master/master_options.h"

#include <glog/logging.h>
#include <gflags/gflags.h>

#include "yb/master/master.h"
#include "yb/master/master_defaults.h"
#include "yb/server/server_base_options.h"
#include "yb/util/flag_tags.h"

using std::make_shared;
using std::vector;

namespace yb {
namespace master {

DEFINE_string(master_addresses, "",
    "Comma-separated list of the host/port RPC addresses of the peer masters. This is needed "
    "for initial cluster create, and is recreated from persisted metadata on master restart."
    "This flag also defaults to empty, which is overloaded to be able to either: "
    "a) start a non-distributed mode master if local instance file is not present. "
    "b) allow for a master to be restarted gracefully and get its peer list from the "
    "local cmeta file of the last committed config, if local instance file is present.");
TAG_FLAG(master_addresses, experimental);
DEFINE_uint64(master_replication_factor, 0,
    "Number of master replicas. By default it is detected based on master_addresses option, but "
    "could be specified explicitly together with passing one or more master service domain name and"
    " port through master_addresses for masters auto-discovery when running on Kubernetes.");

// NOTE: This flag is deprecated.
DEFINE_bool(create_cluster, false,
  "(DEPRECATED). This flag was earlier used to distinguish if the master process is "
  "being started to create a cluster or if this just a restart.");
TAG_FLAG(create_cluster, hidden);

const char* MasterOptions::kServerType = "master";

MasterOptions::MasterOptions(server::MasterAddressesPtr master_addresses)
    : ServerBaseOptions(kMasterDefaultPort) {
  server_type = kServerType;

  SetMasterAddresses(master_addresses);
}

Result<MasterOptions> MasterOptions::CreateMasterOptions() {
  server::MasterAddresses master_addresses;
  std::string master_addresses_resolved_str;
  RETURN_NOT_OK(server::DetermineMasterAddresses(
      "master_addresses", FLAGS_master_addresses, FLAGS_master_replication_factor,
      &master_addresses, &master_addresses_resolved_str));

  MasterOptions opts(std::make_shared<server::MasterAddresses>(std::move(master_addresses)));
  opts.master_addresses_flag = master_addresses_resolved_str;
  return opts;
}

} // namespace master
} // namespace yb
