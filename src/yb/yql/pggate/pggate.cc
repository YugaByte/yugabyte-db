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
//--------------------------------------------------------------------------------------------------

#include <boost/optional.hpp>

#include "yb/client/yb_table_name.h"

#include "yb/common/pg_system_attr.h"

#include "yb/docdb/doc_key.h"
#include "yb/docdb/primitive_value.h"

#include "yb/yql/pggate/pggate.h"
#include "yb/yql/pggate/pggate_flags.h"
#include "yb/yql/pggate/pg_memctx.h"
#include "yb/yql/pggate/pg_ddl.h"
#include "yb/yql/pggate/pg_insert.h"
#include "yb/yql/pggate/pg_update.h"
#include "yb/yql/pggate/pg_delete.h"
#include "yb/yql/pggate/pg_truncate_colocated.h"
#include "yb/yql/pggate/pg_select.h"
#include "yb/yql/pggate/pg_txn_manager.h"
#include "yb/yql/pggate/ybc_pggate.h"

#include "yb/util/flag_tags.h"
#include "yb/client/client_fwd.h"
#include "yb/client/client_utils.h"
#include "yb/client/table.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/secure_stream.h"
#include "yb/server/secure.h"

#include "yb/tserver/tserver_shared_mem.h"

DECLARE_string(rpc_bind_addresses);
DECLARE_bool(use_node_to_node_encryption);
DECLARE_string(certs_dir);

namespace yb {
namespace pggate {

using docdb::PrimitiveValue;
using docdb::ValueType;

namespace {

CHECKED_STATUS AddColumn(PgCreateTable* pg_stmt, const char *attr_name, int attr_num,
                         const YBCPgTypeEntity *attr_type, bool is_hash, bool is_range,
                         bool is_desc, bool is_nulls_first) {
  using SortingType = ColumnSchema::SortingType;
  SortingType sorting_type = SortingType::kNotSpecified;

  if (!is_hash && is_range) {
    if (is_desc) {
      sorting_type = is_nulls_first ? SortingType::kDescending : SortingType::kDescendingNullsLast;
    } else {
      sorting_type = is_nulls_first ? SortingType::kAscending : SortingType::kAscendingNullsLast;
    }
  }

  return pg_stmt->AddColumn(attr_name, attr_num, attr_type, is_hash, is_range, sorting_type);
}

Result<PgApiImpl::MessengerHolder> BuildMessenger(
    const string& client_name,
    int32_t num_reactors,
    const scoped_refptr<MetricEntity>& metric_entity,
    const std::shared_ptr<MemTracker>& parent_mem_tracker) {
  std::unique_ptr<rpc::SecureContext> secure_context;
  if (FLAGS_use_node_to_node_encryption) {
    secure_context = VERIFY_RESULT(server::CreateSecureContext(FLAGS_certs_dir));
  }
  auto messenger = VERIFY_RESULT(client::CreateClientMessenger(
      client_name, num_reactors, metric_entity, parent_mem_tracker, secure_context.get()));
  return PgApiImpl::MessengerHolder{std::move(secure_context), std::move(messenger)};
}

std::unique_ptr<tserver::TServerSharedObject> InitTServerSharedObject() {
  // Do not use shared memory in initdb or if explicity set to be ignored.
  if (YBCIsInitDbModeEnvVarSet() || FLAGS_TEST_pggate_ignore_tserver_shm ||
      FLAGS_pggate_tserver_shm_fd == -1) {
    return nullptr;
  }
  return std::make_unique<tserver::TServerSharedObject>(CHECK_RESULT(
      tserver::TServerSharedObject::OpenReadOnly(FLAGS_pggate_tserver_shm_fd)));
}

Result<std::vector<std::string>> FetchExistingYbctids(PgSession::ScopedRefPtr session,
                                                      PgOid database_id,
                                                      PgOid table_id,
                                                      const std::vector<Slice>& ybctids) {
  auto desc  = VERIFY_RESULT(session->LoadTable(PgObjectId(database_id, table_id)));
  auto read_op = desc->NewPgsqlSelect();
  auto read_req = read_op->mutable_request();
  read_req->set_unknown_ybctid_allowed(true);
  PgsqlExpressionPB* expr_pb = read_req->add_targets();
  expr_pb->set_column_id(to_underlying(PgSystemAttrNum::kYBTupleId));
  auto doc_op = std::make_shared<PgDocReadOp>(session, desc, std::move(read_op));
  // Postgres uses SELECT FOR KEY SHARE query for FK check.
  // Use same lock level.
  PgExecParameters exec_params = doc_op->ExecParameters();
  exec_params.rowmark = ROW_MARK_KEYSHARE;
  RETURN_NOT_OK(doc_op->ExecuteInit(&exec_params));
  RETURN_NOT_OK(static_cast<PgDocOp*>(doc_op.get())->PopulateDmlByYbctidOps(&ybctids));
  RETURN_NOT_OK(doc_op->Execute());
  std::vector<std::string> result;
  result.reserve(ybctids.size());
  std::list<PgDocResult> rowsets;
  do {
    rowsets.clear();
    RETURN_NOT_OK(doc_op->GetResult(&rowsets));
    for (auto& row : rowsets) {
      RETURN_NOT_OK(row.ProcessSystemColumns());
      for (const auto& ybctid : row.ybctids()) {
        result.push_back(ybctid.ToBuffer());
      }
    }
  } while (!rowsets.empty());
  return std::move(result);
}

} // namespace

using std::make_shared;
using client::YBSession;

//--------------------------------------------------------------------------------------------------

PggateOptions::PggateOptions() : ServerBaseOptions(kDefaultPort) {
  server_type = "tserver";
  rpc_opts.connection_keepalive_time_ms = FLAGS_pgsql_rpc_keepalive_time_ms;

  if (FLAGS_pggate_proxy_bind_address.empty()) {
    HostPort host_port;
    CHECK_OK(host_port.ParseString(FLAGS_rpc_bind_addresses, 0));
    host_port.set_port(PggateOptions::kDefaultPort);
    FLAGS_pggate_proxy_bind_address = host_port.ToString();
    LOG(INFO) << "Reset YSQL bind address to " << FLAGS_pggate_proxy_bind_address;
  }
  rpc_opts.rpc_bind_addresses = FLAGS_pggate_proxy_bind_address;
  master_addresses_flag = FLAGS_pggate_master_addresses;

  server::MasterAddresses master_addresses;
  // TODO: we might have to allow setting master_replication_factor similarly to how it is done
  // in tserver to support master auto-discovery on Kubernetes.
  CHECK_OK(server::DetermineMasterAddresses(
      "pggate_master_addresses", master_addresses_flag, /* master_replication_factor */ 0,
      &master_addresses, &master_addresses_flag));
  SetMasterAddresses(make_shared<server::MasterAddresses>(std::move(master_addresses)));
}

//--------------------------------------------------------------------------------------------------

PgApiImpl::PgApiImpl(const YBCPgTypeEntity *YBCDataTypeArray, int count, YBCPgCallbacks callbacks)
    : metric_registry_(new MetricRegistry()),
      metric_entity_(METRIC_ENTITY_server.Instantiate(metric_registry_.get(), "yb.pggate")),
      mem_tracker_(MemTracker::CreateTracker("PostgreSQL")),
      messenger_holder_(CHECK_RESULT(BuildMessenger("pggate_ybclient",
                                                    FLAGS_pggate_ybclient_reactor_threads,
                                                    metric_entity_,
                                                    mem_tracker_))),
      async_client_init_(messenger_holder_.messenger.get()->name(),
                         FLAGS_pggate_ybclient_reactor_threads,
                         FLAGS_pggate_rpc_timeout_secs,
                         "" /* tserver_uuid */,
                         &pggate_options_,
                         metric_entity_,
                         mem_tracker_,
                         messenger_holder_.messenger.get()),
      clock_(new server::HybridClock()),
      tserver_shared_object_(InitTServerSharedObject()),
      pg_txn_manager_(new PgTxnManager(&async_client_init_, clock_, tserver_shared_object_.get())),
      pg_callbacks_(callbacks) {
  CHECK_OK(clock_->Init());

  // Setup type mapping.
  for (int idx = 0; idx < count; idx++) {
    const YBCPgTypeEntity *type_entity = &YBCDataTypeArray[idx];
    type_map_[type_entity->type_oid] = type_entity;
  }

  async_client_init_.Start();
}

PgApiImpl::~PgApiImpl() {
  messenger_holder_.messenger->Shutdown();
  async_client_init_.client()->Shutdown();
}

const YBCPgTypeEntity *PgApiImpl::FindTypeEntity(int type_oid) {
  const auto iter = type_map_.find(type_oid);
  if (iter != type_map_.end()) {
    return iter->second;
  }
  return nullptr;
}

//--------------------------------------------------------------------------------------------------

Status PgApiImpl::CreateEnv(PgEnv **pg_env) {
  *pg_env = pg_env_.get();
  return Status::OK();
}

Status PgApiImpl::DestroyEnv(PgEnv *pg_env) {
  pg_env_ = nullptr;
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

Status PgApiImpl::InitSession(const PgEnv *pg_env,
                              const string& database_name) {
  CHECK(!pg_session_);
  auto session = make_scoped_refptr<PgSession>(client(),
                                               database_name,
                                               pg_txn_manager_,
                                               clock_,
                                               tserver_shared_object_.get(),
                                               pg_callbacks_);
  if (!database_name.empty()) {
    RETURN_NOT_OK(session->ConnectDatabase(database_name));
  }

  pg_session_.swap(session);
  return Status::OK();
}

Status PgApiImpl::InvalidateCache() {
  pg_session_->InvalidateCache();
  return Status::OK();
}

const bool PgApiImpl::GetDisableTransparentCacheRefreshRetry() {
  return FLAGS_TEST_ysql_disable_transparent_cache_refresh_retry;
}

//--------------------------------------------------------------------------------------------------

PgMemctx *PgApiImpl::CreateMemctx() {
  // Postgres will create YB Memctx when it first use the Memctx to allocate YugaByte object.
  return PgMemctx::Create();
}

Status PgApiImpl::DestroyMemctx(PgMemctx *memctx) {
  // Postgres will destroy YB Memctx by releasing the pointer.
  return PgMemctx::Destroy(memctx);
}

Status PgApiImpl::ResetMemctx(PgMemctx *memctx) {
  // Postgres reset YB Memctx when clearing a context content without clearing its nested context.
  return PgMemctx::Reset(memctx);
}

// TODO(neil) Use Arena in the future.
// - PgStatement should have been declared as derived class of "MCBase".
// - All objects of PgStatement's derived class should be allocated by YbPgMemctx::Arena.
// - We cannot use Arena yet because quite a large number of YugaByte objects are being referenced
//   from other layers.  Those added code violated the original design as they assume ScopedPtr
//   instead of memory pool is being used. This mess should be cleaned up later.
//
// For now, statements is allocated as ScopedPtr and cached in the memory context. The statements
// would then be destructed when the context is destroyed and all other references are also cleared.
Status PgApiImpl::AddToCurrentPgMemctx(std::unique_ptr<PgStatement> stmt,
                                       PgStatement **handle) {
  *handle = stmt.get();
  pg_callbacks_.GetCurrentYbMemctx()->Register(stmt.release());
  return Status::OK();
}

// TODO(neil) Most like we don't need table_desc. If we do need it, use Arena here.
// - PgTableDesc should have been declared as derived class of "MCBase".
// - PgTableDesc objects should be allocated by YbPgMemctx::Arena.
//
// For now, table_desc is allocated as ScopedPtr and cached in the memory context. The table_desc
// would then be destructed when the context is destroyed.
Status PgApiImpl::AddToCurrentPgMemctx(size_t table_desc_id,
                                       const PgTableDesc::ScopedRefPtr &table_desc) {
  pg_callbacks_.GetCurrentYbMemctx()->Cache(table_desc_id, table_desc);
  return Status::OK();
}

Status PgApiImpl::GetTabledescFromCurrentPgMemctx(size_t table_desc_id, PgTableDesc **handle) {
  pg_callbacks_.GetCurrentYbMemctx()->GetCache(table_desc_id, handle);
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

Status PgApiImpl::CreateSequencesDataTable() {
  return pg_session_->CreateSequencesDataTable();
}

Status PgApiImpl::InsertSequenceTuple(int64_t db_oid,
                                      int64_t seq_oid,
                                      uint64_t ysql_catalog_version,
                                      int64_t last_val,
                                      bool is_called) {
  return pg_session_->InsertSequenceTuple(
      db_oid, seq_oid, ysql_catalog_version, last_val, is_called);
}

Status PgApiImpl::UpdateSequenceTupleConditionally(int64_t db_oid,
                                                   int64_t seq_oid,
                                                   uint64_t ysql_catalog_version,
                                                   int64_t last_val,
                                                   bool is_called,
                                                   int64_t expected_last_val,
                                                   bool expected_is_called,
                                                   bool *skipped) {
  return pg_session_->UpdateSequenceTuple(
      db_oid, seq_oid, ysql_catalog_version, last_val, is_called,
      expected_last_val, expected_is_called, skipped);
}

Status PgApiImpl::UpdateSequenceTuple(int64_t db_oid,
                                      int64_t seq_oid,
                                      uint64_t ysql_catalog_version,
                                      int64_t last_val,
                                      bool is_called,
                                      bool* skipped) {
  return pg_session_->UpdateSequenceTuple(
      db_oid, seq_oid, ysql_catalog_version, last_val,
      is_called, boost::none, boost::none, skipped);
}

Status PgApiImpl::ReadSequenceTuple(int64_t db_oid,
                                    int64_t seq_oid,
                                    uint64_t ysql_catalog_version,
                                    int64_t *last_val,
                                    bool *is_called) {
  return pg_session_->ReadSequenceTuple(db_oid, seq_oid, ysql_catalog_version, last_val, is_called);
}

Status PgApiImpl::DeleteSequenceTuple(int64_t db_oid, int64_t seq_oid) {
  return pg_session_->DeleteSequenceTuple(db_oid, seq_oid);
}


//--------------------------------------------------------------------------------------------------

void PgApiImpl::DeleteStatement(PgStatement *handle) {
  if (handle) {
    PgMemctx::Destroy(handle);
  }
}

Status PgApiImpl::ClearBinds(PgStatement *handle) {
  return handle->ClearBinds();
}

//--------------------------------------------------------------------------------------------------

Status PgApiImpl::ConnectDatabase(const char *database_name) {
  return pg_session_->ConnectDatabase(database_name);
}

Status PgApiImpl::IsDatabaseColocated(const PgOid database_oid, bool *colocated) {
  return pg_session_->IsDatabaseColocated(database_oid, colocated);
}

Status PgApiImpl::NewCreateDatabase(const char *database_name,
                                    const PgOid database_oid,
                                    const PgOid source_database_oid,
                                    const PgOid next_oid,
                                    const bool colocated,
                                    PgStatement **handle) {
  auto stmt = std::make_unique<PgCreateDatabase>(pg_session_, database_name, database_oid,
                                                 source_database_oid, next_oid, colocated);
  if (pg_txn_manager_->IsDdlMode()) {
    stmt->AddTransaction(pg_txn_manager_->GetDdlTxnMetadata());
  }
  RETURN_NOT_OK(AddToCurrentPgMemctx(std::move(stmt), handle));
  return Status::OK();
}

Status PgApiImpl::ExecCreateDatabase(PgStatement *handle) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_CREATE_DATABASE)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }

  return down_cast<PgCreateDatabase*>(handle)->Exec();
}

Status PgApiImpl::NewDropDatabase(const char *database_name,
                                  PgOid database_oid,
                                  PgStatement **handle) {
  auto stmt = std::make_unique<PgDropDatabase>(pg_session_, database_name, database_oid);
  RETURN_NOT_OK(AddToCurrentPgMemctx(std::move(stmt), handle));
  return Status::OK();
}

Status PgApiImpl::ExecDropDatabase(PgStatement *handle) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_DROP_DATABASE)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  return down_cast<PgDropDatabase*>(handle)->Exec();
}

Status PgApiImpl::NewAlterDatabase(const char *database_name,
                                  PgOid database_oid,
                                  PgStatement **handle) {
  auto stmt = std::make_unique<PgAlterDatabase>(pg_session_, database_name, database_oid);
  RETURN_NOT_OK(AddToCurrentPgMemctx(std::move(stmt), handle));
  return Status::OK();
}

Status PgApiImpl::AlterDatabaseRenameDatabase(PgStatement *handle, const char *newname) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_ALTER_DATABASE)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  return down_cast<PgAlterDatabase*>(handle)->RenameDatabase(newname);
}

Status PgApiImpl::ExecAlterDatabase(PgStatement *handle) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_ALTER_DATABASE)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  return down_cast<PgAlterDatabase*>(handle)->Exec();
}

Status PgApiImpl::ReserveOids(const PgOid database_oid,
                              const PgOid next_oid,
                              const uint32_t count,
                              PgOid *begin_oid,
                              PgOid *end_oid) {
  return pg_session_->ReserveOids(database_oid, next_oid, count, begin_oid, end_oid);
}

Status PgApiImpl::GetCatalogMasterVersion(uint64_t *version) {
  return pg_session_->GetCatalogMasterVersion(version);
}

Result<PgTableDesc::ScopedRefPtr> PgApiImpl::LoadTable(const PgObjectId& table_id) {
  return pg_session_->LoadTable(table_id);
}

void PgApiImpl::InvalidateTableCache(const PgObjectId& table_id) {
  pg_session_->InvalidateTableCache(table_id);
}

//--------------------------------------------------------------------------------------------------

Status PgApiImpl::NewCreateTablegroup(const char *database_name,
                                      const PgOid database_oid,
                                      const PgOid tablegroup_oid,
                                      PgStatement **handle) {
  auto stmt = std::make_unique<PgCreateTablegroup>(pg_session_, database_name,
                                                   database_oid, tablegroup_oid);
  RETURN_NOT_OK(AddToCurrentPgMemctx(std::move(stmt), handle));
  return Status::OK();
}

Status PgApiImpl::ExecCreateTablegroup(PgStatement *handle) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_CREATE_TABLEGROUP)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }

  return down_cast<PgCreateTablegroup*>(handle)->Exec();
}

Status PgApiImpl::NewDropTablegroup(const PgOid database_oid,
                                    const PgOid tablegroup_oid,
                                    PgStatement **handle) {
  auto stmt = std::make_unique<PgDropTablegroup>(pg_session_, database_oid, tablegroup_oid);
  RETURN_NOT_OK(AddToCurrentPgMemctx(std::move(stmt), handle));
  return Status::OK();
}


Status PgApiImpl::ExecDropTablegroup(PgStatement *handle) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_DROP_TABLEGROUP)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  return down_cast<PgDropTablegroup*>(handle)->Exec();
}


//--------------------------------------------------------------------------------------------------

Status PgApiImpl::NewCreateTable(const char *database_name,
                                 const char *schema_name,
                                 const char *table_name,
                                 const PgObjectId& table_id,
                                 bool is_shared_table,
                                 bool if_not_exist,
                                 bool add_primary_key,
                                 const bool colocated,
                                 const PgObjectId& tablegroup_oid,
                                 PgStatement **handle) {
  auto stmt = std::make_unique<PgCreateTable>(
      pg_session_, database_name, schema_name, table_name,
      table_id, is_shared_table, if_not_exist, add_primary_key, colocated, tablegroup_oid);
  if (pg_txn_manager_->IsDdlMode()) {
    stmt->AddTransaction(pg_txn_manager_->GetDdlTxnMetadata());
  }
  RETURN_NOT_OK(AddToCurrentPgMemctx(std::move(stmt), handle));
  return Status::OK();
}

Status PgApiImpl::CreateTableAddColumn(PgStatement *handle, const char *attr_name, int attr_num,
                                       const YBCPgTypeEntity *attr_type,
                                       bool is_hash, bool is_range,
                                       bool is_desc, bool is_nulls_first) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_CREATE_TABLE)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  return AddColumn(down_cast<PgCreateTable*>(handle), attr_name, attr_num, attr_type,
      is_hash, is_range, is_desc, is_nulls_first);
}

Status PgApiImpl::CreateTableSetNumTablets(PgStatement *handle, int32_t num_tablets) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_CREATE_TABLE)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  return down_cast<PgCreateTable*>(handle)->SetNumTablets(num_tablets);
}

Status PgApiImpl::AddSplitBoundary(PgStatement *handle, PgExpr **exprs, int expr_count) {
  // Partitioning a TABLE or an INDEX.
  if (PgStatement::IsValidStmt(handle, StmtOp::STMT_CREATE_TABLE) ||
      PgStatement::IsValidStmt(handle, StmtOp::STMT_CREATE_INDEX)) {
    return down_cast<PgCreateTable*>(handle)->AddSplitBoundary(exprs, expr_count);
  }

  // Invalid handle.
  return STATUS(InvalidArgument, "Invalid statement handle");
}

Status PgApiImpl::ExecCreateTable(PgStatement *handle) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_CREATE_TABLE)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  return down_cast<PgCreateTable*>(handle)->Exec();
}

Status PgApiImpl::NewAlterTable(const PgObjectId& table_id,
                                PgStatement **handle) {
  auto stmt = std::make_unique<PgAlterTable>(pg_session_, table_id);
  RETURN_NOT_OK(AddToCurrentPgMemctx(std::move(stmt), handle));
  return Status::OK();
}

Status PgApiImpl::AlterTableAddColumn(PgStatement *handle, const char *name,
                                      int order, const YBCPgTypeEntity *attr_type) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_ALTER_TABLE)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }

  PgAlterTable *pg_stmt = down_cast<PgAlterTable*>(handle);
  return pg_stmt->AddColumn(name, attr_type, order);
}

Status PgApiImpl::AlterTableRenameColumn(PgStatement *handle, const char *oldname,
                                         const char *newname) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_ALTER_TABLE)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }

  PgAlterTable *pg_stmt = down_cast<PgAlterTable*>(handle);
  return pg_stmt->RenameColumn(oldname, newname);
}

Status PgApiImpl::AlterTableDropColumn(PgStatement *handle, const char *name) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_ALTER_TABLE)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }

  PgAlterTable *pg_stmt = down_cast<PgAlterTable*>(handle);
  return pg_stmt->DropColumn(name);
}

Status PgApiImpl::AlterTableRenameTable(PgStatement *handle, const char *db_name,
                                        const char *newname) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_ALTER_TABLE)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }

  PgAlterTable *pg_stmt = down_cast<PgAlterTable*>(handle);
  return pg_stmt->RenameTable(db_name, newname);
}

Status PgApiImpl::ExecAlterTable(PgStatement *handle) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_ALTER_TABLE)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  PgAlterTable *pg_stmt = down_cast<PgAlterTable*>(handle);
  return pg_stmt->Exec();
}

Status PgApiImpl::NewDropTable(const PgObjectId& table_id,
                               bool if_exist,
                               PgStatement **handle) {
  auto stmt = std::make_unique<PgDropTable>(pg_session_, table_id, if_exist);
  RETURN_NOT_OK(AddToCurrentPgMemctx(std::move(stmt), handle));
  return Status::OK();
}

Status PgApiImpl::NewTruncateTable(const PgObjectId& table_id,
                                   PgStatement **handle) {
  auto stmt = std::make_unique<PgTruncateTable>(pg_session_, table_id);
  RETURN_NOT_OK(AddToCurrentPgMemctx(std::move(stmt), handle));
  return Status::OK();
}

Status PgApiImpl::ExecTruncateTable(PgStatement *handle) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_TRUNCATE_TABLE)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  return down_cast<PgTruncateTable*>(handle)->Exec();
}

Status PgApiImpl::GetTableDesc(const PgObjectId& table_id,
                               PgTableDesc **handle) {
  // First read from memory context.
  size_t hash_id = hash_value(table_id);
  RETURN_NOT_OK(GetTabledescFromCurrentPgMemctx(hash_id, handle));

  // Read from environment.
  if (*handle == nullptr) {
    auto result = pg_session_->LoadTable(table_id);
    RETURN_NOT_OK(result);
    RETURN_NOT_OK(AddToCurrentPgMemctx(hash_id, *result));

    *handle = result->get();
  }

  return Status::OK();
}

Status PgApiImpl::GetColumnInfo(YBCPgTableDesc table_desc,
                                int16_t attr_number,
                                bool *is_primary,
                                bool *is_hash) {
  return table_desc->GetColumnInfo(attr_number, is_primary, is_hash);
}

Status PgApiImpl::DmlModifiesRow(PgStatement *handle, bool *modifies_row) {
  if (!handle) {
    return STATUS(InvalidArgument, "Invalid statement handle");
  }

  *modifies_row = false;

  switch (handle->stmt_op()) {
    case StmtOp::STMT_UPDATE:
    case StmtOp::STMT_DELETE:
      *modifies_row = true;
      break;
    default:
      break;
  }

  return Status::OK();
}

Status PgApiImpl::SetIsSysCatalogVersionChange(PgStatement *handle) {
  if (!handle) {
    return STATUS(InvalidArgument, "Invalid statement handle");
  }

  switch (handle->stmt_op()) {
    case StmtOp::STMT_UPDATE:
    case StmtOp::STMT_DELETE:
    case StmtOp::STMT_INSERT:
      down_cast<PgDmlWrite *>(handle)->SetIsSystemCatalogChange();
      return Status::OK();
    default:
      break;
  }

  return STATUS(InvalidArgument, "Invalid statement handle");
}

Status PgApiImpl::SetCatalogCacheVersion(PgStatement *handle, uint64_t catalog_cache_version) {
  if (!handle) {
    return STATUS(InvalidArgument, "Invalid statement handle");
  }

  switch (handle->stmt_op()) {
    case StmtOp::STMT_SELECT:
    case StmtOp::STMT_INSERT:
    case StmtOp::STMT_UPDATE:
    case StmtOp::STMT_DELETE:
      down_cast<PgDml *>(handle)->SetCatalogCacheVersion(catalog_cache_version);
      return Status::OK();
    default:
      break;
  }

  return STATUS(InvalidArgument, "Invalid statement handle");
}

//--------------------------------------------------------------------------------------------------

Status PgApiImpl::NewCreateIndex(const char *database_name,
                                 const char *schema_name,
                                 const char *index_name,
                                 const PgObjectId& index_id,
                                 const PgObjectId& base_table_id,
                                 bool is_shared_index,
                                 bool is_unique_index,
                                 const bool skip_index_backfill,
                                 bool if_not_exist,
                                 const PgObjectId& tablegroup_oid,
                                 PgStatement **handle) {
  auto stmt = std::make_unique<PgCreateIndex>(
      pg_session_, database_name, schema_name, index_name, index_id, base_table_id,
      is_shared_index, is_unique_index, skip_index_backfill, if_not_exist, tablegroup_oid);
  RETURN_NOT_OK(AddToCurrentPgMemctx(std::move(stmt), handle));
  return Status::OK();
}

Status PgApiImpl::CreateIndexAddColumn(PgStatement *handle, const char *attr_name, int attr_num,
                                       const YBCPgTypeEntity *attr_type,
                                       bool is_hash, bool is_range,
                                       bool is_desc, bool is_nulls_first) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_CREATE_INDEX)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }

  return AddColumn(down_cast<PgCreateIndex*>(handle), attr_name, attr_num, attr_type,
      is_hash, is_range, is_desc, is_nulls_first);
}

Status PgApiImpl::CreateIndexSetNumTablets(PgStatement *handle, int32_t num_tablets) {
  SCHECK(PgStatement::IsValidStmt(handle, StmtOp::STMT_CREATE_INDEX),
         InvalidArgument,
         "Invalid statement handle");
  return down_cast<PgCreateIndex*>(handle)->SetNumTablets(num_tablets);
}

Status PgApiImpl::ExecCreateIndex(PgStatement *handle) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_CREATE_INDEX)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  return down_cast<PgCreateIndex*>(handle)->Exec();
}

Status PgApiImpl::NewDropIndex(const PgObjectId& index_id,
                               bool if_exist,
                               PgStatement **handle) {
  auto stmt = std::make_unique<PgDropIndex>(pg_session_, index_id, if_exist);
  RETURN_NOT_OK(AddToCurrentPgMemctx(std::move(stmt), handle));
  return Status::OK();
}

Result<IndexPermissions> PgApiImpl::WaitUntilIndexPermissionsAtLeast(
    const PgObjectId& table_id,
    const PgObjectId& index_id,
    const IndexPermissions& target_index_permissions) {
  return pg_session_->WaitUntilIndexPermissionsAtLeast(
      table_id,
      index_id,
      target_index_permissions);
}

Status PgApiImpl::AsyncUpdateIndexPermissions(const PgObjectId& indexed_table_id) {
  return pg_session_->AsyncUpdateIndexPermissions(indexed_table_id);
}

Status PgApiImpl::ExecPostponedDdlStmt(PgStatement *handle) {
  if (!handle) {
    return STATUS(InvalidArgument, "Invalid statement handle");
  }

  switch (handle->stmt_op()) {
    case StmtOp::STMT_DROP_TABLE:
      return down_cast<PgDropTable*>(handle)->Exec();
    case StmtOp::STMT_DROP_INDEX:
      return down_cast<PgDropIndex*>(handle)->Exec();

    default:
      break;
  }
  return STATUS(InvalidArgument, "Invalid statement handle");
}

Status PgApiImpl::BackfillIndex(const PgObjectId& table_id) {
  return pg_session_->BackfillIndex(table_id);
}

//--------------------------------------------------------------------------------------------------
// DML Statment Support.
//--------------------------------------------------------------------------------------------------

// Binding -----------------------------------------------------------------------------------------

Status PgApiImpl::DmlAppendTarget(PgStatement *handle, PgExpr *target) {
  return down_cast<PgDml*>(handle)->AppendTarget(target);
}

Status PgApiImpl::DmlBindColumn(PgStatement *handle, int attr_num, PgExpr *attr_value) {
  return down_cast<PgDml*>(handle)->BindColumn(attr_num, attr_value);
}

Status PgApiImpl::DmlBindColumnCondEq(PgStatement *handle, int attr_num, PgExpr *attr_value) {
  return down_cast<PgDmlRead*>(handle)->BindColumnCondEq(attr_num, attr_value);
}

Status PgApiImpl::DmlBindColumnCondBetween(PgStatement *handle, int attr_num, PgExpr *attr_value,
    PgExpr *attr_value_end) {
  return down_cast<PgDmlRead*>(handle)->BindColumnCondBetween(attr_num, attr_value, attr_value_end);
}

Status PgApiImpl::DmlBindColumnCondIn(PgStatement *handle, int attr_num, int n_attr_values,
    PgExpr **attr_values) {
  return down_cast<PgDmlRead*>(handle)->BindColumnCondIn(attr_num, n_attr_values, attr_values);
}

Status PgApiImpl::DmlBindTable(PgStatement *handle) {
  return down_cast<PgDml*>(handle)->BindTable();
}

CHECKED_STATUS PgApiImpl::DmlAssignColumn(PgStatement *handle, int attr_num, PgExpr *attr_value) {
  return down_cast<PgDml*>(handle)->AssignColumn(attr_num, attr_value);
}

Status PgApiImpl::DmlFetch(PgStatement *handle, int32_t natts, uint64_t *values, bool *isnulls,
                           PgSysColumns *syscols, bool *has_data) {
  return down_cast<PgDml*>(handle)->Fetch(natts, values, isnulls, syscols, has_data);
}

Status PgApiImpl::ProcessYBTupleId(const YBCPgYBTupleIdDescriptor& descr,
                                   const YBTupleIdProcessor& processor) {
  auto target_desc = VERIFY_RESULT(pg_session_->LoadTable(
      PgObjectId(descr.database_oid, descr.table_oid)));
  SCHECK_EQ(descr.nattrs, target_desc->num_key_columns(), Corruption,
            "Number of key components does not match column description");
  vector<PrimitiveValue> *values = nullptr;
  PgsqlExpressionPB *expr_pb;
  PgsqlExpressionPB temp_expr_pb;
  google::protobuf::RepeatedPtrField<PgsqlExpressionPB> hashed_values;
  vector<docdb::PrimitiveValue> hashed_components, range_components;
  hashed_components.reserve(target_desc->num_hash_key_columns());
  range_components.reserve(
      target_desc->num_hash_key_columns() - target_desc->num_hash_key_columns());
  size_t remain_attr = descr.nattrs;
  // DocDB API requires that partition columns must be listed in their created-order.
  // Order from target_desc should be used as attributes sequence may have different order.
  for (const auto& c : target_desc->columns()) {
    for (auto attr = descr.attrs, end = descr.attrs + descr.nattrs; attr != end; ++attr) {
      if (attr->attr_num == c.attr_num()) {
        if (!c.desc()->is_primary()) {
          return STATUS_SUBSTITUTE(
              InvalidArgument, "Attribute number $0 not a primary attribute", attr->attr_num);
        }
        if (c.desc()->is_partition()) {
          // Hashed component.
          values = &hashed_components;
          expr_pb = hashed_values.Add();
        } else {
          // Range component.
          values = &range_components;
          expr_pb = &temp_expr_pb;
        }

        if (attr->is_null) {
          values->emplace_back(ValueType::kNullLow);
        } else {
          if (attr->attr_num == to_underlying(PgSystemAttrNum::kYBRowId)) {
            expr_pb->mutable_value()->set_binary_value(pg_session_->GenerateNewRowid());
          } else {
            PgConstant value(attr->type_entity, attr->datum, false);
            SCHECK_EQ(c.internal_type(), value.internal_type(), Corruption,
                      "Attribute value type does not match column type");
            RETURN_NOT_OK(value.Eval(expr_pb->mutable_value()));
          }
          values->push_back(PrimitiveValue::FromQLValuePB(expr_pb->value(),
                                                          c.desc()->sorting_type()));
        }

        if (--remain_attr == 0) {
          SCHECK_EQ(hashed_components.size(), target_desc->num_hash_key_columns(), Corruption,
                    "Number of hashed components does not match column description");
          SCHECK_EQ(range_components.size(),
                    target_desc->num_key_columns() - target_desc->num_hash_key_columns(),
                    Corruption, "Number of range components does not match column description");
          if (hashed_values.empty()) {
            return processor(docdb::DocKey(move(range_components)).Encode());
          }
          string partition_key;
          const PartitionSchema& partition_schema = target_desc->table()->partition_schema();
          RETURN_NOT_OK(partition_schema.EncodeKey(hashed_values, &partition_key));
          const uint16_t hash = PartitionSchema::DecodeMultiColumnHashValue(partition_key);

          return processor(
              docdb::DocKey(hash, move(hashed_components), move(range_components)).Encode());
        }
        break;
      }
    }
  }

  return STATUS_FORMAT(Corruption, "Not all attributes ($0) were resolved", remain_attr);
}

void PgApiImpl::StartOperationsBuffering() {
  pg_session_->StartOperationsBuffering();
}

Status PgApiImpl::StopOperationsBuffering() {
  return pg_session_->StopOperationsBuffering();
}

Status PgApiImpl::ResetOperationsBuffering() {
  return pg_session_->ResetOperationsBuffering();
}

Status PgApiImpl::FlushBufferedOperations() {
  return pg_session_->FlushBufferedOperations();
}

void PgApiImpl::DropBufferedOperations() {
  pg_session_->DropBufferedOperations();
}

Status PgApiImpl::DmlExecWriteOp(PgStatement *handle, int32_t *rows_affected_count) {
  switch (handle->stmt_op()) {
    case StmtOp::STMT_INSERT:
    case StmtOp::STMT_UPDATE:
    case StmtOp::STMT_DELETE:
    case StmtOp::STMT_TRUNCATE:
      {
        auto dml_write = down_cast<PgDmlWrite *>(handle);
        RETURN_NOT_OK(dml_write->Exec(rows_affected_count != nullptr /* force_non_bufferable */));
        if (rows_affected_count) {
          *rows_affected_count = dml_write->GetRowsAffectedCount();
        }
        return Status::OK();
      }
    default:
      break;
  }
  return STATUS(InvalidArgument, "Invalid statement handle");
}

// Insert ------------------------------------------------------------------------------------------

Status PgApiImpl::NewInsert(const PgObjectId& table_id,
                            const bool is_single_row_txn,
                            PgStatement **handle) {
  *handle = nullptr;
  auto stmt = std::make_unique<PgInsert>(pg_session_, table_id, is_single_row_txn);
  RETURN_NOT_OK(stmt->Prepare());
  RETURN_NOT_OK(AddToCurrentPgMemctx(std::move(stmt), handle));
  return Status::OK();
}

Status PgApiImpl::ExecInsert(PgStatement *handle) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_INSERT)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  return down_cast<PgInsert*>(handle)->Exec();
}

Status PgApiImpl::InsertStmtSetUpsertMode(PgStatement *handle) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_INSERT)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  down_cast<PgInsert*>(handle)->SetUpsertMode();

  return Status::OK();
}

Status PgApiImpl::InsertStmtSetWriteTime(PgStatement *handle, const HybridTime write_time) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_INSERT)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  RETURN_NOT_OK(down_cast<PgInsert*>(handle)->SetWriteTime(write_time));
  return Status::OK();
}

Status PgApiImpl::InsertStmtSetIsBackfill(PgStatement *handle, const bool is_backfill) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_INSERT)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  down_cast<PgInsert*>(handle)->SetIsBackfill(is_backfill);
  return Status::OK();
}

// Update ------------------------------------------------------------------------------------------

Status PgApiImpl::NewUpdate(const PgObjectId& table_id,
                            const bool is_single_row_txn,
                            PgStatement **handle) {
  *handle = nullptr;
  auto stmt = std::make_unique<PgUpdate>(pg_session_, table_id, is_single_row_txn);
  RETURN_NOT_OK(stmt->Prepare());
  RETURN_NOT_OK(AddToCurrentPgMemctx(std::move(stmt), handle));
  return Status::OK();
}

Status PgApiImpl::ExecUpdate(PgStatement *handle) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_UPDATE)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  return down_cast<PgUpdate*>(handle)->Exec();
}

// Delete ------------------------------------------------------------------------------------------

Status PgApiImpl::NewDelete(const PgObjectId& table_id,
                            const bool is_single_row_txn,
                            PgStatement **handle) {
  *handle = nullptr;
  auto stmt = std::make_unique<PgDelete>(pg_session_, table_id, is_single_row_txn);
  RETURN_NOT_OK(stmt->Prepare());
  RETURN_NOT_OK(AddToCurrentPgMemctx(std::move(stmt), handle));
  return Status::OK();
}

Status PgApiImpl::ExecDelete(PgStatement *handle) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_DELETE)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  return down_cast<PgDelete*>(handle)->Exec();
}

// Colocated Truncate ------------------------------------------------------------------------------

Status PgApiImpl::NewTruncateColocated(const PgObjectId& table_id,
                                       const bool is_single_row_txn,
                                       PgStatement **handle) {
  *handle = nullptr;
  auto stmt = std::make_unique<PgTruncateColocated>(pg_session_, table_id, is_single_row_txn);
  RETURN_NOT_OK(stmt->Prepare());
  RETURN_NOT_OK(AddToCurrentPgMemctx(std::move(stmt), handle));
  return Status::OK();
}

Status PgApiImpl::ExecTruncateColocated(PgStatement *handle) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_TRUNCATE)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  return down_cast<PgTruncateColocated*>(handle)->Exec();
}

// Select ------------------------------------------------------------------------------------------

Status PgApiImpl::NewSelect(const PgObjectId& table_id,
                            const PgObjectId& index_id,
                            const PgPrepareParameters *prepare_params,
                            PgStatement **handle) {
  // Scenarios:
  // - Sequential Scan: PgSelect to read from table_id.
  // - Primary Scan: PgSelect from table_id. YugaByte does not have separate table for primary key.
  // - Index-Only-Scan: PgSelectIndex directly from secondary index_id.
  // - IndexScan: Use PgSelectIndex to read from index_id and then PgSelect to read from table_id.
  //     Note that for SysTable, only one request is send for both table_id and index_id.
  *handle = nullptr;
  std::unique_ptr<PgDmlRead> stmt;
  if (prepare_params && prepare_params->index_only_scan && prepare_params->use_secondary_index) {
    if (!index_id.IsValid()) {
      return STATUS(InvalidArgument, "Cannot run query with invalid index ID");
    }
    stmt = std::make_unique<PgSelectIndex>(pg_session_, table_id, index_id, prepare_params);
  } else {
    // For IndexScan PgSelect processing will create subquery PgSelectIndex.
    stmt = std::make_unique<PgSelect>(pg_session_, table_id, index_id, prepare_params);
  }

  RETURN_NOT_OK(stmt->Prepare());
  RETURN_NOT_OK(AddToCurrentPgMemctx(std::move(stmt), handle));
  return Status::OK();
}

Status PgApiImpl::SetForwardScan(PgStatement *handle, bool is_forward_scan) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_SELECT)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  down_cast<PgDmlRead*>(handle)->SetForwardScan(is_forward_scan);
  return Status::OK();
}

Status PgApiImpl::ExecSelect(PgStatement *handle, const PgExecParameters *exec_params) {
  if (!PgStatement::IsValidStmt(handle, StmtOp::STMT_SELECT)) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  return down_cast<PgDmlRead*>(handle)->Exec(exec_params);
}

//--------------------------------------------------------------------------------------------------
// Expressions.
//--------------------------------------------------------------------------------------------------

// Column references -------------------------------------------------------------------------------

Status PgApiImpl::NewColumnRef(PgStatement *stmt, int attr_num, const PgTypeEntity *type_entity,
                               const PgTypeAttrs *type_attrs, PgExpr **expr_handle) {
  if (!stmt) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  PgColumnRef::SharedPtr colref = make_shared<PgColumnRef>(attr_num, type_entity, type_attrs);
  stmt->AddExpr(colref);

  *expr_handle = colref.get();
  return Status::OK();
}

// Constant ----------------------------------------------------------------------------------------
Status PgApiImpl::NewConstant(YBCPgStatement stmt, const YBCPgTypeEntity *type_entity,
                              uint64_t datum, bool is_null, YBCPgExpr *expr_handle) {
  if (!stmt) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  PgExpr::SharedPtr pg_const = make_shared<PgConstant>(type_entity, datum, is_null);
  stmt->AddExpr(pg_const);

  *expr_handle = pg_const.get();
  return Status::OK();
}

Status PgApiImpl::NewConstantVirtual(YBCPgStatement stmt, const YBCPgTypeEntity *type_entity,
                                     YBCPgDatumKind datum_kind, YBCPgExpr *expr_handle) {
  if (!stmt) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  PgExpr::SharedPtr pg_const = make_shared<PgConstant>(type_entity, datum_kind);
  stmt->AddExpr(pg_const);

  *expr_handle = pg_const.get();
  return Status::OK();
}

Status PgApiImpl::NewConstantOp(YBCPgStatement stmt, const YBCPgTypeEntity *type_entity,
                                uint64_t datum, bool is_null, YBCPgExpr *expr_handle, bool is_gt) {
  if (!stmt) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  PgExpr::SharedPtr pg_const = make_shared<PgConstant>(type_entity, datum, is_null,
      is_gt ? PgExpr::Opcode::PG_EXPR_GT : PgExpr::Opcode::PG_EXPR_LT);
  stmt->AddExpr(pg_const);

  *expr_handle = pg_const.get();
  return Status::OK();
}

// Text constant -----------------------------------------------------------------------------------

Status PgApiImpl::UpdateConstant(PgExpr *expr, const char *value, bool is_null) {
  if (expr->opcode() != PgExpr::Opcode::PG_EXPR_CONSTANT) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid expression handle for constant");
  }
  down_cast<PgConstant*>(expr)->UpdateConstant(value, is_null);
  return Status::OK();
}

Status PgApiImpl::UpdateConstant(PgExpr *expr, const void *value, int64_t bytes, bool is_null) {
  if (expr->opcode() != PgExpr::Opcode::PG_EXPR_CONSTANT) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid expression handle for constant");
  }
  down_cast<PgConstant*>(expr)->UpdateConstant(value, bytes, is_null);
  return Status::OK();
}

// Text constant -----------------------------------------------------------------------------------

Status PgApiImpl::NewOperator(PgStatement *stmt, const char *opname,
                              const YBCPgTypeEntity *type_entity,
                              PgExpr **op_handle) {
  if (!stmt) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid statement handle");
  }
  RETURN_NOT_OK(PgExpr::CheckOperatorName(opname));

  // Create operator.
  PgExpr::SharedPtr pg_op = make_shared<PgOperator>(opname, type_entity);
  stmt->AddExpr(pg_op);

  *op_handle = pg_op.get();
  return Status::OK();
}

Status PgApiImpl::OperatorAppendArg(PgExpr *op_handle, PgExpr *arg) {
  if (!op_handle || !arg) {
    // Invalid handle.
    return STATUS(InvalidArgument, "Invalid expression handle");
  }
  down_cast<PgOperator*>(op_handle)->AppendArg(arg);
  return Status::OK();
}

Result<bool> PgApiImpl::IsInitDbDone() {
  return pg_session_->IsInitDbDone();
}

Result<uint64_t> PgApiImpl::GetSharedCatalogVersion() {
  return pg_session_->GetSharedCatalogVersion();
}

Result<uint64_t> PgApiImpl::GetSharedAuthKey() {
  return pg_session_->GetSharedAuthKey();
}

// Transaction Control -----------------------------------------------------------------------------
Status PgApiImpl::BeginTransaction() {
  pg_session_->InvalidateForeignKeyReferenceCache();
  return pg_txn_manager_->BeginTransaction();
}

Status PgApiImpl::RecreateTransaction() {
  pg_session_->InvalidateForeignKeyReferenceCache();
  return pg_txn_manager_->RecreateTransaction();
}

Status PgApiImpl::RestartTransaction() {
  pg_session_->InvalidateForeignKeyReferenceCache();
  return pg_txn_manager_->RestartTransaction();
}

Status PgApiImpl::CommitTransaction() {
  pg_session_->InvalidateForeignKeyReferenceCache();
  return pg_txn_manager_->CommitTransaction();
}

Status PgApiImpl::AbortTransaction() {
  pg_session_->InvalidateForeignKeyReferenceCache();
  return pg_txn_manager_->AbortTransaction();
}

Status PgApiImpl::SetTransactionIsolationLevel(int isolation) {
  return pg_txn_manager_->SetIsolationLevel(isolation);
}

Status PgApiImpl::SetTransactionReadOnly(bool read_only) {
  return pg_txn_manager_->SetReadOnly(read_only);
}

Status PgApiImpl::SetTransactionDeferrable(bool deferrable) {
  return pg_txn_manager_->SetDeferrable(deferrable);
}

Status PgApiImpl::EnterSeparateDdlTxnMode() {
  // Flush all buffered operations as ddl txn use its own transaction session.
  RETURN_NOT_OK(pg_session_->FlushBufferedOperations());
  return pg_txn_manager_->EnterSeparateDdlTxnMode();
}

Status PgApiImpl::ExitSeparateDdlTxnMode(bool success) {
  // Flush all buffered operations as ddl txn use its own transaction session.
  if (success) {
    RETURN_NOT_OK(pg_session_->FlushBufferedOperations());
  } else {
    pg_session_->DropBufferedOperations();
  }

  return pg_txn_manager_->ExitSeparateDdlTxnMode(success);
}

Result<bool> PgApiImpl::ForeignKeyReferenceExists(
    PgOid table_id, const Slice& ybctid, PgOid database_id) {
  return pg_session_->ForeignKeyReferenceExists(
      table_id, ybctid, std::bind(FetchExistingYbctids,
                                  pg_session_,
                                  database_id,
                                  std::placeholders::_1,
                                  std::placeholders::_2));
}

void PgApiImpl::AddForeignKeyReferenceIntent(PgOid table_id, const Slice& ybctid) {
  pg_session_->AddForeignKeyReferenceIntent(table_id, ybctid);
}

void PgApiImpl::DeleteForeignKeyReference(PgOid table_id, const Slice& ybctid) {
  pg_session_->DeleteForeignKeyReference(table_id, ybctid);
}

void PgApiImpl::SetTimeout(const int timeout_ms) {
  pg_session_->SetTimeout(timeout_ms);
}

} // namespace pggate
} // namespace yb
