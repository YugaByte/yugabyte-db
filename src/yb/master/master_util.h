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

#ifndef YB_MASTER_MASTER_UTIL_H
#define YB_MASTER_MASTER_UTIL_H

#include <memory>

#include "yb/rpc/rpc_fwd.h"
#include "yb/master/master.pb.h"

#include "yb/util/monotime.h"
#include "yb/util/net/net_util.h"
#include "yb/util/status.h"

// This file contains utility functions that can be shared between client and master code.

namespace yb {

namespace consensus {

class RaftPeerPB;

}

namespace master {

// Given a hostport, return the master server information protobuf.
// Does not apply to tablet server.
CHECKED_STATUS GetMasterEntryForHosts(
    rpc::ProxyCache* proxy_cache,
    const std::vector<HostPort>& hostports,
    MonoDelta timeout,
    ServerEntryPB* e);

const HostPortPB& DesiredHostPort(const TSInfoPB& ts_info, const CloudInfoPB& from);

void TakeRegistration(consensus::RaftPeerPB* source, TSInfoPB* dest);
void CopyRegistration(const consensus::RaftPeerPB& source, TSInfoPB* dest);

void TakeRegistration(ServerRegistrationPB* source, TSInfoPB* dest);
void CopyRegistration(const ServerRegistrationPB& source, TSInfoPB* dest);

bool IsSystemNamespace(const std::string& namespace_name);

YQLDatabase GetDefaultDatabaseType(const std::string& keyspace_name);

template<class PB>
YQLDatabase GetDatabaseType(const PB& ns) {
  return ns.has_database_type() ? ns.database_type() : GetDefaultDatabaseType(ns.name());
}

YQLDatabase GetDatabaseTypeForTable(const TableType table_type);
TableType GetTableTypeForDatabase(const YQLDatabase database_type);

} // namespace master
} // namespace yb

#endif // YB_MASTER_MASTER_UTIL_H
