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
#ifndef YB_CLIENT_CLIENT_BUILDER_INTERNAL_H_
#define YB_CLIENT_CLIENT_BUILDER_INTERNAL_H_

#include <functional>
#include <string>
#include <vector>

#include "yb/client/client.h"
#include "yb/common/entity_ids.h"
#include "yb/common/wire_protocol.h"
#include "yb/gutil/ref_counted.h"

namespace yb {

namespace client {

class YBClientBuilder::Data {
 public:
  Data();
  ~Data();

  // Flag name to fetch master addresses from flagfile.
  std::string master_address_flag_name_;

  // This is a REST endpoint from which the list of master hosts and ports can be queried. This
  // takes precedence over both 'master_server_addrs_file_' and 'master_server_addrs_'.
  std::string master_server_endpoint_;

  // This vector holds the list of master server addresses. Note that each entry in this vector
  // can either be a single 'host:port' or a comma separated list of 'host1:port1,host2:port2,...'.
  std::vector<std::string> master_server_addrs_;

  // This bool determines whether to use FLAGS_flagfile as an override of client-entered data.
  bool skip_master_flagfile_ = false;

  int32_t num_reactors_ = 0;

  MonoDelta default_admin_operation_timeout_;
  MonoDelta default_rpc_timeout_;

  // Metric entity to be used by components emitting metrics.
  scoped_refptr<MetricEntity> metric_entity_;

  // A descriptive name for the client. Useful for embedded ybclients.
  std::string client_name_ = "ybclient";

  // The size of the threadpool to use for calling callbacks.
  size_t threadpool_size_ = 0;

  // If all masters are available but no leader is present on client init,
  // this flag determines if the client returns failure right away
  // or waits for a leader to be elected.
  bool wait_for_leader_election_on_init_ = true;

  // Placement information for the client.
  CloudInfoPB cloud_info_pb_;

  // When the client is part of a CQL proxy, this denotes the uuid for the associated tserver to
  // aid in detecting local tservers.
  TabletServerId uuid_;

  std::shared_ptr<MemTracker> parent_mem_tracker_;

  bool skip_master_leader_resolution_ = false;

  // See YBClient::Data::master_address_sources_
  std::vector<MasterAddressSource> master_address_sources_;
 private:
  DISALLOW_COPY_AND_ASSIGN(Data);
};

}  // namespace client
}  // namespace yb

#endif // YB_CLIENT_CLIENT_BUILDER_INTERNAL_H_
