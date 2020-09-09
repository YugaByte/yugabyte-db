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

#ifndef YB_COMMON_ENTITY_IDS_H
#define YB_COMMON_ENTITY_IDS_H

#include <string>
#include <set>
#include <utility>

#include "yb/util/result.h"
#include "yb/util/strongly_typed_string.h"

namespace yb {

// TODO: switch many of these to opaque types for additional type safety and efficiency.

using NamespaceName = std::string;
using TableName = std::string;
using UDTypeName = std::string;
using RoleName = std::string;

using NamespaceId = std::string;
using TableId = std::string;
using UDTypeId = std::string;
using CDCStreamId = std::string;

using PeerId = std::string;
using SnapshotId = std::string;
using TabletServerId = PeerId;
using TabletId = std::string;
using TablegroupId = std::string;

YB_STRONGLY_TYPED_STRING(KvStoreId);

// TODO(#79): switch to YB_STRONGLY_TYPED_STRING
using RaftGroupId = std::string;

using NamespaceIdTableNamePair = std::pair<NamespaceId, TableName>;

using FlushRequestId = std::string;

using RedisConfigKey = std::string;

static const uint32_t kPgSequencesDataTableOid = 0xFFFF;
static const uint32_t kPgSequencesDataDatabaseOid = 0xFFFF;

static const uint32_t kPgIndexTableOid = 2610;  // Hardcoded for pg_index. (in pg_index.h)

extern const TableId kPgProcTableId;

// Get YB namespace id for a Postgres database.
NamespaceId GetPgsqlNamespaceId(uint32_t database_oid);

// Get YB table id for a Postgres table.
TableId GetPgsqlTableId(uint32_t database_oid, uint32_t table_oid);

// Get YB tablegroup id for a Postgres tablegroup.
TablegroupId GetPgsqlTablegroupId(uint32_t database_oid, uint32_t tablegroup_oid);

// Is the namespace/table id a Postgres database or table id?
bool IsPgsqlId(const string& id);

// Get Postgres database and table oids from a YB namespace/table id.
Result<uint32_t> GetPgsqlDatabaseOid(const NamespaceId& namespace_id);
Result<uint32_t> GetPgsqlTableOid(const TableId& table_id);
Result<uint32_t> GetPgsqlTablegroupOid(const TablegroupId& tablegroup_id);
Result<uint32_t> GetPgsqlTablegroupOidByTableId(const TableId& table_id);
Result<uint32_t> GetPgsqlDatabaseOidByTableId(const TableId& table_id);

}  // namespace yb

#endif  // YB_COMMON_ENTITY_IDS_H
