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

#include "yb/master/yql_peers_vtable.h"

#include "yb/common/wire_protocol.h"

#include "yb/master/ts_descriptor.h"

#include "yb/rpc/messenger.h"

#include "yb/util/net/dns_resolver.h"

namespace yb {
namespace master {

using std::string;
using std::shared_ptr;
using std::unique_ptr;
using std::map;

namespace {

const std::string kPeer = "peer";
const std::string kDataCenter = "data_center";
const std::string kHostId = "host_id";
const std::string kPreferredIp = "preferred_ip";
const std::string kRack = "rack";
const std::string kReleaseVersion = "release_version";
const std::string kRPCAddress = "rpc_address";
const std::string kSchemaVersion = "schema_version";
const std::string kTokens = "tokens";

} // namespace

PeersVTable::PeersVTable(const Master* const master)
    : YQLVirtualTable(master::kSystemPeersTableName, master, CreateSchema()) {
}

Result<std::shared_ptr<QLRowBlock>> PeersVTable::RetrieveData(
    const QLReadRequestPB& request) const {
  // Retrieve all lives nodes known by the master.
  // TODO: Ideally we would like to populate this table with all valid nodes of the cluster, but
  // currently the master just has a list of all nodes it has heard from and which one of those
  // are dead. As a result, the master can't distinguish between nodes that are part of the
  // cluster and are dead vs nodes that have been removed from the cluster. Since, we might
  // change the cluster topology often, for now its safe to just have the live nodes here.
  vector<shared_ptr<TSDescriptor> > descs;
  GetSortedLiveDescriptors(&descs);

  auto& resolver = master_->messenger()->resolver();

  // Collect all unique ip addresses.
  InetAddress remote_endpoint(VERIFY_RESULT(resolver.Resolve(request.remote_endpoint().host())));

  const auto& proxy_uuid = request.proxy_uuid();

  // Populate the YQL rows.
  auto vtable = std::make_shared<QLRowBlock>(schema_);

  struct Entry {
    size_t index;
    TSInformationPB ts_info;
    util::PublicPrivateIPFutures ts_ips;
  };

  std::vector<Entry> entries;
  entries.reserve(descs.size());

  size_t index = 0;
  for (const auto& desc : descs) {
    size_t current_index = index++;

    // This is thread safe since all operations are reads.
    TSInformationPB ts_info = *desc->GetTSInformationPB();

    if (!proxy_uuid.empty()) {
      if (desc->permanent_uuid() == proxy_uuid) {
        continue;
      }
    } else {
      // In case of old proxy, fallback to old endpoint based mechanism.
      if (util::RemoteEndpointMatchesTServer(ts_info, remote_endpoint)) {
        continue;
      }
    }

    entries.push_back({current_index, std::move(ts_info)});
    auto& entry = entries.back();
    entry.ts_ips = util::GetPublicPrivateIPFutures(entry.ts_info, &resolver);
  }

  for (const auto& entry : entries) {
    // The system.peers table has one entry for each of its peers, whereas there is no entry for
    // the node that the CQL client connects to. In this case, this node is the 'remote_endpoint'
    // in QLReadRequestPB since that is address of the CQL proxy which sent this request. As a
    // result, skip 'remote_endpoint' in the results.
    auto private_ip = entry.ts_ips.private_ip_future.get();
    if (!private_ip.ok()) {
      LOG(ERROR) << "Failed to get private ip from " << entry.ts_info.ShortDebugString()
                 << ": " << private_ip.status();
      continue;
    }

    auto public_ip = entry.ts_ips.public_ip_future.get();
    if (!public_ip.ok()) {
      LOG(ERROR) << "Failed to get public ip from " << entry.ts_info.ShortDebugString()
                 << ": " << public_ip.status();
      continue;
    }

    // Need to use only 1 rpc address per node since system.peers has only 1 entry for each host,
    // so pick the first one.
    QLRow &row = vtable->Extend();
    RETURN_NOT_OK(SetColumnValue(kPeer, InetAddress(*public_ip), &row));
    RETURN_NOT_OK(SetColumnValue(kRPCAddress, InetAddress(*public_ip), &row));
    RETURN_NOT_OK(SetColumnValue(kPreferredIp, InetAddress(*private_ip), &row));

    // Datacenter and rack.
    CloudInfoPB cloud_info = entry.ts_info.registration().common().cloud_info();
    RETURN_NOT_OK(SetColumnValue(kDataCenter, cloud_info.placement_region(), &row));
    RETURN_NOT_OK(SetColumnValue(kRack, cloud_info.placement_zone(), &row));

    // HostId.
    Uuid host_id;
    RETURN_NOT_OK(host_id.FromHexString(entry.ts_info.tserver_instance().permanent_uuid()));
    RETURN_NOT_OK(SetColumnValue(kHostId, host_id, &row));
    RETURN_NOT_OK(SetColumnValue(yb::master::kSystemTablesReleaseVersionColumn,
        yb::master::kSystemTablesReleaseVersion, &row));

    // schema_version.
    Uuid schema_version;
    RETURN_NOT_OK(schema_version.FromString(master::kDefaultSchemaVersion));
    RETURN_NOT_OK(SetColumnValue(kSchemaVersion, schema_version, &row));

    // Tokens.
    RETURN_NOT_OK(SetColumnValue(
        kTokens, util::GetTokensValue(entry.index, descs.size()), &row));
  }

  return vtable;
}

Schema PeersVTable::CreateSchema() const {
  SchemaBuilder builder;
  CHECK_OK(builder.AddHashKeyColumn(kPeer, QLType::Create(DataType::INET)));
  CHECK_OK(builder.AddColumn(kDataCenter, QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddColumn(kHostId, QLType::Create(DataType::UUID)));
  CHECK_OK(builder.AddColumn(kPreferredIp, QLType::Create(DataType::INET)));
  CHECK_OK(builder.AddColumn(kRack, QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddColumn(kReleaseVersion, QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddColumn(kRPCAddress, QLType::Create(DataType::INET)));
  CHECK_OK(builder.AddColumn(kSchemaVersion, QLType::Create(DataType::UUID)));
  CHECK_OK(builder.AddColumn(kTokens, QLType::CreateTypeSet(DataType::STRING)));
  return builder.Build();
}

}  // namespace master
}  // namespace yb
