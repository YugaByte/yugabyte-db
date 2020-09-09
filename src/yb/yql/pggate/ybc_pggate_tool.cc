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

#include "yb/yql/pggate/ybc_pggate_tool.h"

#include "yb/common/ybc-internal.h"
#include "yb/yql/pggate/ybc_pggate.h"

#include "yb/util/memory/mc_types.h"
#include "yb/yql/pggate/pg_env.h"
#include "yb/yql/pggate/pggate_flags.h"

namespace yb {
namespace pggate {

namespace {

// Fetches relation's unique constraint name to specified buffer.
// If relation is not an index and it has primary key the name of primary key index is returned.
// In other cases, relation name is used.
//
// Not implemented for tools.
void FetchUniqueConstraintName(PgOid relation_id, char* dest, size_t max_size) {
  CHECK(false) << "Not implemented";
}

YBCPgMemctx GetCurrentToolYbMemctx() {
  static YBCPgMemctx tool_memctx = nullptr;

  if (!tool_memctx) {
    tool_memctx = YBCPgCreateMemctx();
  }
  return tool_memctx;
}

// Conversion Table.
// Contain function pointers for conversion between PostgreSQL Datum to YugaByte data.
// Currently it is not used in the tools and can be empty.
static const YBCPgTypeEntity YBCEmptyTypeEntityTable[] = {};

} // anonymous namespace

//--------------------------------------------------------------------------------------------------
// C API.
//--------------------------------------------------------------------------------------------------
extern "C" {

void YBCSetMasterAddresses(const char* hosts) {
  LOG(INFO) << "Setting custom master addresses: " << hosts;
  FLAGS_pggate_master_addresses = hosts;
}

YBCStatus YBCInitPgGateBackend() {
  YBCPgCallbacks callbacks;
  callbacks.FetchUniqueConstraintName = &FetchUniqueConstraintName;
  callbacks.GetCurrentYbMemctx = &GetCurrentToolYbMemctx;
  YBCInitPgGate(YBCEmptyTypeEntityTable, 0, callbacks);

  return YBCPgInitSession(/* pg_env */ nullptr, /* database_name */ nullptr);
}

void YBCShutdownPgGateBackend() {
  YBCDestroyPgGate();
  YBCPgDestroyMemctx(GetCurrentToolYbMemctx());
}

} // extern "C"

} // namespace pggate
} // namespace yb
