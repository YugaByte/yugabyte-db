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

#include "yb/master/yql_partitions_vtable.h"

#include "yb/common/ql_value.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/master_util.h"
#include "yb/rpc/messenger.h"
#include "yb/util/net/dns_resolver.h"

DECLARE_int32(partitions_vtable_cache_refresh_secs);

DEFINE_bool(use_cache_for_partitions_vtable, true,
            "Whether we should use caching for system.partitions table.");

namespace yb {
namespace master {

namespace {

const std::string kKeyspaceName = "keyspace_name";
const std::string kTableName = "table_name";
const std::string kStartKey = "start_key";
const std::string kEndKey = "end_key";
const std::string kId = "id";
const std::string kReplicaAddresses = "replica_addresses";

}

YQLPartitionsVTable::YQLPartitionsVTable(const TableName& table_name,
                                         const NamespaceName& namespace_name,
                                         Master * const master)
    : YQLVirtualTable(table_name, namespace_name, master, CreateSchema()) {
}

Result<std::shared_ptr<QLRowBlock>> YQLPartitionsVTable::RetrieveData(
    const QLReadRequestPB& request) const {
  // The cached versions are initialized to -1, so if there is a race, we may still generate the
  // cache on the calling thread.
  if (FLAGS_partitions_vtable_cache_refresh_secs > 0 &&
      cached_tablets_version_ >= 0 &&
      cached_tablet_locations_version_ >= 0) {
    // Don't need a version match here, since we have a bg task handling cache refreshing.
    return cache_;
  }

  RETURN_NOT_OK(GenerateAndCacheData());
  return cache_;
}

Status YQLPartitionsVTable::GenerateAndCacheData() const {
  CatalogManager* catalog_manager = master_->catalog_manager();
  {
    std::shared_lock<boost::shared_mutex> lock(mutex_);
    if (FLAGS_use_cache_for_partitions_vtable &&
        catalog_manager->tablets_version() == cached_tablets_version_ &&
        catalog_manager->tablet_locations_version() == cached_tablet_locations_version_) {
      // Cache is up to date, so we could use it.
      return Status::OK();
    }
  }

  std::lock_guard<boost::shared_mutex> lock(mutex_);
  auto new_tablets_version = catalog_manager->tablets_version();
  auto new_tablet_locations_version = catalog_manager->tablet_locations_version();
  if (FLAGS_use_cache_for_partitions_vtable &&
      new_tablets_version == cached_tablets_version_ &&
      new_tablet_locations_version == cached_tablet_locations_version_) {
    // Cache was updated between locks, and now it is up to date.
    return Status::OK();
  }

  auto vtable = std::make_shared<QLRowBlock>(schema_);
  std::vector<scoped_refptr<TableInfo> > tables;
  catalog_manager->GetAllTables(&tables, true /* includeOnlyRunningTables */);
  auto& resolver = master_->messenger()->resolver();

  std::unordered_map<std::string, std::shared_future<Result<IpAddress>>> dns_lookups;
  struct TabletData {
    scoped_refptr<NamespaceInfo> namespace_info;
    scoped_refptr<TableInfo> table;
    scoped_refptr<TabletInfo> tablet;
    TabletLocationsPB* locations;
  };
  std::vector<TabletData> tablets;
  google::protobuf::Arena arena;

  for (const scoped_refptr<TableInfo>& table : tables) {
    // Skip non-YQL tables.
    if (!CatalogManager::IsYcqlTable(*table)) {
      continue;
    }

    // Get namespace for table.
    NamespaceIdentifierPB namespace_id;
    namespace_id.set_id(table->namespace_id());
    scoped_refptr<NamespaceInfo> namespace_info;
    RETURN_NOT_OK(catalog_manager->FindNamespace(namespace_id, &namespace_info));

    // Get tablets for table.
    std::vector<scoped_refptr<TabletInfo>> tablet_infos;
    table->GetAllTablets(&tablet_infos);
    for (const auto& info : tablet_infos) {
      tablets.emplace_back();
      auto& data = tablets.back();
      data.namespace_info = namespace_info;
      data.table = table;
      data.tablet = info;
      data.locations = google::protobuf::Arena::Create<TabletLocationsPB>(&arena);
      auto s = catalog_manager->GetTabletLocations(info, data.locations);
      if (!s.ok()) {
        data.locations->Clear();
      }
      for (const auto& replica : data.locations->replicas()) {
        auto host = DesiredHostPort(replica.ts_info(), CloudInfoPB()).host();
        if (dns_lookups.count(host) == 0) {
          dns_lookups.emplace(host, resolver.ResolveFuture(host));
        }
      }
    }
  }

  std::unordered_map<std::string, InetAddress> dns_results;

  for (auto& p : dns_lookups) {
    dns_results.emplace(p.first, InetAddress(VERIFY_RESULT(p.second.get())));
  }

  // Reserve upfront memory, as we're likely to need to insert a row for each tablet.
  vtable->Reserve(tablets.size());
  for (const auto& data : tablets) {
    // Skip not-found tablets: they might not be running yet or have been deleted.
    if (data.locations->table_id().empty()) {
      continue;
    }

    QLRow& row = vtable->Extend();
    RETURN_NOT_OK(SetColumnValue(kKeyspaceName, data.namespace_info->name(), &row));
    RETURN_NOT_OK(SetColumnValue(kTableName, data.table->name(), &row));

    const PartitionPB& partition = data.locations->partition();
    RETURN_NOT_OK(SetColumnValue(kStartKey, partition.partition_key_start(), &row));
    RETURN_NOT_OK(SetColumnValue(kEndKey, partition.partition_key_end(), &row));

    // Note: tablet id is in host byte order.
    Uuid uuid;
    RETURN_NOT_OK(uuid.FromHexString(data.tablet->id()));
    RETURN_NOT_OK(SetColumnValue(kId, uuid, &row));

    // Get replicas for tablet.
    QLValuePB replica_addresses;
    QLMapValuePB *map_value = replica_addresses.mutable_map_value();
    for (const auto& replica : data.locations->replicas()) {
      auto host = DesiredHostPort(replica.ts_info(), CloudInfoPB()).host();
      QLValue::set_inetaddress_value(dns_results[host], map_value->add_keys());

      map_value->add_values()->set_string_value(consensus::RaftPeerPB::Role_Name(replica.role()));
    }
    RETURN_NOT_OK(SetColumnValue(kReplicaAddresses, replica_addresses, &row));
  }

  // Update cache and versions.
  cached_tablets_version_ = new_tablets_version;
  cached_tablet_locations_version_ = new_tablet_locations_version;
  cache_ = vtable;

  return Status::OK();
}

Schema YQLPartitionsVTable::CreateSchema() const {
  SchemaBuilder builder;
  CHECK_OK(builder.AddHashKeyColumn(kKeyspaceName, QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddKeyColumn(kTableName, QLType::Create(DataType::STRING)));
  CHECK_OK(builder.AddKeyColumn(kStartKey, QLType::Create(DataType::BINARY)));
  CHECK_OK(builder.AddColumn(kEndKey, QLType::Create(DataType::BINARY)));
  CHECK_OK(builder.AddColumn(kId, QLType::Create(DataType::UUID)));
  CHECK_OK(builder.AddColumn(kReplicaAddresses,
                             QLType::CreateTypeMap(DataType::INET, DataType::STRING)));
  return builder.Build();
}

}  // namespace master
}  // namespace yb
