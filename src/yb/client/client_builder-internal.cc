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

#include "yb/client/client_builder-internal.h"
#include "yb/util/metrics.h"

DEFINE_int32(
    yb_client_num_reactors, 16,
    "Number of reactor threads for the yb client to communicate with different tservers.");

DEFINE_int32(
    yb_client_admin_operation_timeout_sec, 120,
    "The number of seconds after which an admin operation should timeout.");

namespace yb {

namespace client {

YBClientBuilder::Data::Data()
    : master_address_flag_name_("tserver_master_addrs"),
      num_reactors_(FLAGS_yb_client_num_reactors),
      default_admin_operation_timeout_(
          MonoDelta::FromSeconds(FLAGS_yb_client_admin_operation_timeout_sec)),
      default_rpc_timeout_(MonoDelta::FromSeconds(60)) {}

YBClientBuilder::Data::~Data() {
}

}  // namespace client
}  // namespace yb
