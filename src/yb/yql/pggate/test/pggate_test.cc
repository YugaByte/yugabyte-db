//--------------------------------------------------------------------------------------------------
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
//--------------------------------------------------------------------------------------------------

#include "yb/yql/pggate/test/pggate_test.h"

#include <gflags/gflags.h>

#include "yb/yql/pggate/pg_session.h"
#include "yb/yql/pggate/pg_memctx.h"
#include "yb/yql/pggate/pggate_flags.h"

DECLARE_string(pggate_master_addresses);
DECLARE_string(test_leave_files);

namespace yb {
namespace pggate {
namespace {

extern "C" void FetchUniqueConstraintName(PgOid relation_id, char* dest, size_t max_size) {
  CHECK(false) << "Not implemented";
}

} // namespace

YBCPgMemctx test_memctx = nullptr;
static YBCPgMemctx TestGetCurrentYbMemctx() {
  if (!test_memctx) {
    test_memctx = YBCPgCreateMemctx();
  }
  return test_memctx;
}

PggateTest::PggateTest() {
}

PggateTest::~PggateTest() {
  CHECK_YBC_STATUS(YBCPgDestroyMemctx(test_memctx));
}

//--------------------------------------------------------------------------------------------------
// Error handling routines.
void PggateTest::CheckYBCStatus(YBCStatus status, const char* file_name, int line_number) {
  CHECK_OK(Status(status, AddRef::kTrue));
}

void *PggateTestAlloc(size_t bytes) {
  static MemoryContext memctx;
  return static_cast<void*>(memctx.AllocateBytes(bytes));
}

// This implementation is different from what PostgreSQL's cstring_to_text_with_len function does.
// Here we just copy the given string and add a terminating zero. This is what our test expects.
struct varlena* PggateTestCStringToTextWithLen(const char* c, int size) {
  static MemoryContext memctx;
  CHECK_GE(size, 0);
  CHECK_LE(size, 1024ll * 1024 * 1024 - 4);

  char* buf = static_cast<char*>(memctx.AllocateBytes(size + 1));
  memcpy(buf, c, size);
  buf[size] = 0;
  return reinterpret_cast<struct varlena*>(buf);
}

//--------------------------------------------------------------------------------------------------
// Starting and ending routines.

void PggateTest::SetUp() {
  FLAGS_test_leave_files = "always";
  YBTest::SetUp();
}

void PggateTest::TearDown() {
  // Destroy the client before shutting down servers.
  YBCDestroyPgGate();

  // Destroy all servers.
  if (cluster_ != nullptr) {
    cluster_->Shutdown();
    cluster_ = nullptr;
  }
  YBTest::TearDown();
}

Status PggateTest::Init(const char *test_name, int num_tablet_servers) {
  // Create cluster before setting client API.
  RETURN_NOT_OK(CreateCluster(num_tablet_servers));

  // Init PgGate API.
  CHECK_YBC_STATUS(YBCInit(test_name, PggateTestAlloc, PggateTestCStringToTextWithLen));

  const YBCPgTypeEntity *type_table = nullptr;
  int count = 0;
  YBCTestGetTypeTable(&type_table, &count);
  YBCPgCallbacks callbacks;
  callbacks.FetchUniqueConstraintName = &FetchUniqueConstraintName;
  callbacks.GetCurrentYbMemctx = &TestGetCurrentYbMemctx;
  YBCInitPgGate(type_table, count, callbacks);

  // Don't try to connect to tserver shared memory in pggate tests.
  FLAGS_TEST_pggate_ignore_tserver_shm = true;

  // Setup session.
  CHECK_YBC_STATUS(YBCPgInitSession(nullptr /* pg_env */, nullptr /* database_name */));

  // Setup database
  SetupDB();
  return Status::OK();
}

Status PggateTest::CreateCluster(int num_tablet_servers) {
  // Start mini-cluster with given number of tservers (default: 3).
  ExternalMiniClusterOptions opts;
  opts.num_tablet_servers = num_tablet_servers;
  opts.data_root_counter = 0;
  cluster_ = std::make_shared<ExternalMiniCluster>(opts);
  CHECK_OK(cluster_->Start());

  // Setup master address to construct YBClient.
  FLAGS_pggate_master_addresses = cluster_->GetMasterAddresses();

  // Sleep to make sure the cluster is ready before accepting client messages.
  sleep(1);
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

void PggateTest::SetupDB(const string& db_name, const YBCPgOid db_oid) {
  CreateDB(db_name, db_oid);
  ConnectDB(db_name);
}

void PggateTest::CreateDB(const string& db_name, const YBCPgOid db_oid) {
  YBCPgStatement pg_stmt;
  CHECK_YBC_STATUS(YBCPgNewCreateDatabase(
      db_name.c_str(), db_oid, 0 /* source_database_oid */, 0 /* next_oid */, false /* colocated */,
      &pg_stmt));
  CHECK_YBC_STATUS(YBCPgExecCreateDatabase(pg_stmt));
}

void PggateTest::ConnectDB(const string& db_name) {
  CHECK_YBC_STATUS(YBCPgConnectDatabase(db_name.c_str()));
}

void PggateTest::CommitTransaction() {
  CHECK_YBC_STATUS(YBCPgCommitTransaction());
}

// ------------------------------------------------------------------------------------------------
// Make sure that DataType in common.proto matches the YBCPgDataType enum
// TODO: find a better way to generate these enums.

static_assert(static_cast<int>(DataType::UNKNOWN_DATA) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_UNKNOWN_DATA),
              "DataType::UNKNOWN_DATA does not match YBCPgDataType::UNKNOWN_DATA");

static_assert(static_cast<int>(DataType::NULL_VALUE_TYPE) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_NULL_VALUE_TYPE),
              "DataType::NULL_VALUE_TYPE does not match YBCPgDataType::NULL_VALUE_TYPE");

static_assert(static_cast<int>(DataType::INT8) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_INT8),
              "DataType::INT8 does not match YBCPgDataType::INT8");

static_assert(static_cast<int>(DataType::INT16) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_INT16),
              "DataType::INT16 does not match YBCPgDataType::INT16");

static_assert(static_cast<int>(DataType::INT32) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_INT32),
              "DataType::INT32 does not match YBCPgDataType::INT32");

static_assert(static_cast<int>(DataType::INT64) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_INT64),
              "DataType::INT64 does not match YBCPgDataType::INT64");

static_assert(static_cast<int>(DataType::STRING) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_STRING),
              "DataType::STRING does not match YBCPgDataType::STRING");

static_assert(static_cast<int>(DataType::BOOL) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_BOOL),
              "DataType::BOOL does not match YBCPgDataType::BOOL");

static_assert(static_cast<int>(DataType::FLOAT) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_FLOAT),
              "DataType::FLOAT does not match YBCPgDataType::FLOAT");

static_assert(static_cast<int>(DataType::DOUBLE) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_DOUBLE),
              "DataType::DOUBLE does not match YBCPgDataType::DOUBLE");

static_assert(static_cast<int>(DataType::BINARY) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_BINARY),
              "DataType::BINARY does not match YBCPgDataType::BINARY");

static_assert(static_cast<int>(DataType::TIMESTAMP) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_TIMESTAMP),
              "DataType::TIMESTAMP does not match YBCPgDataType::TIMESTAMP");

static_assert(static_cast<int>(DataType::DECIMAL) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_DECIMAL),
              "DataType::DECIMAL does not match YBCPgDataType::DECIMAL");

static_assert(static_cast<int>(DataType::VARINT) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_VARINT),
              "DataType::VARINT does not match YBCPgDataType::VARINT");

static_assert(static_cast<int>(DataType::INET) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_INET),
              "DataType::INET does not match YBCPgDataType::INET");

static_assert(static_cast<int>(DataType::LIST) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_LIST),
              "DataType::LIST does not match YBCPgDataType::LIST");

static_assert(static_cast<int>(DataType::MAP) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_MAP),
              "DataType::MAP does not match YBCPgDataType::MAP");

static_assert(static_cast<int>(DataType::SET) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_SET),
              "DataType::SET does not match YBCPgDataType::SET");

static_assert(static_cast<int>(DataType::UUID) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_UUID),
              "DataType::UUID does not match YBCPgDataType::UUID");

static_assert(static_cast<int>(DataType::TIMEUUID) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_TIMEUUID),
              "DataType::TIMEUUID does not match YBCPgDataType::TIMEUUID");

static_assert(static_cast<int>(DataType::TUPLE) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_TUPLE),
              "DataType::TUPLE does not match YBCPgDataType::TUPLE");

static_assert(static_cast<int>(DataType::TYPEARGS) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_TYPEARGS),
              "DataType::TYPEARGS does not match YBCPgDataType::TYPEARGS");

static_assert(static_cast<int>(DataType::USER_DEFINED_TYPE) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_USER_DEFINED_TYPE),
              "DataType::USER_DEFINED_TYPE does not match YBCPgDataType::USER_DEFINED_TYPE");

static_assert(static_cast<int>(DataType::FROZEN) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_FROZEN),
              "DataType::FROZEN does not match YBCPgDataType::FROZEN");

static_assert(static_cast<int>(DataType::DATE) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_DATE),
              "DataType::DATE does not match YBCPgDataType::DATE");

static_assert(static_cast<int>(DataType::TIME) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_TIME),
              "DataType::TIME does not match YBCPgDataType::TIME");

static_assert(static_cast<int>(DataType::JSONB) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_JSONB),
              "DataType::JSONB does not match YBCPgDataType::JSONB");

static_assert(static_cast<int>(DataType::UINT8) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_UINT8),
              "DataType::UINT8 does not match YBCPgDataType::UINT8");

static_assert(static_cast<int>(DataType::UINT16) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_UINT16),
              "DataType::UINT16 does not match YBCPgDataType::UINT16");

static_assert(static_cast<int>(DataType::UINT32) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_UINT32),
              "DataType::UINT32 does not match YBCPgDataType::UINT32");

static_assert(static_cast<int>(DataType::UINT64) ==
                  static_cast<int>(YBCPgDataType::YB_YQL_DATA_TYPE_UINT64),
              "DataType::UINT64 does not match YBCPgDataType::UINT64");

// End of data type enum consistency checking
// ------------------------------------------------------------------------------------------------

} // namespace pggate
} // namespace yb
