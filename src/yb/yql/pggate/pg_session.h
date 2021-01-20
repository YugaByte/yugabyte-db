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

#ifndef YB_YQL_PGGATE_PG_SESSION_H_
#define YB_YQL_PGGATE_PG_SESSION_H_

#include <unordered_set>

#include <boost/optional.hpp>
#include <boost/unordered_set.hpp>

#include "yb/client/client_fwd.h"

#include "yb/common/transaction.h"

#include "yb/gutil/ref_counted.h"

#include "yb/server/hybrid_clock.h"

#include "yb/tserver/tserver_util_fwd.h"

#include "yb/util/oid_generator.h"
#include "yb/util/result.h"

#include "yb/yql/pggate/pg_env.h"
#include "yb/yql/pggate/pg_tabledesc.h"

namespace yb {
namespace pggate {

YB_STRONGLY_TYPED_BOOL(OpBuffered);

class PgTxnManager;
class PgSession;

// Convenience typedefs.
struct BufferableOperation {
  std::shared_ptr<client::YBPgsqlOp> operation;
  // Postgres's relation id. Required to resolve constraint name in case
  // operation will fail with PGSQL_STATUS_DUPLICATE_KEY_ERROR.
  PgObjectId relation_id;
};

typedef std::vector<BufferableOperation> PgsqlOpBuffer;

// This class provides access to run operation's result by reading std::future<Status>
// and analyzing possible pending errors of YBSession object in GetStatus() method.
// If GetStatus() method will not be called, possible errors in YBSession object will be preserved.
// Also GetStatus() checks results of buffered operations (if any) executed in same RPC.
class PgSessionAsyncRunResult {
 public:
  using StatusProvider = std::function<Status()>;

  PgSessionAsyncRunResult() = default;
  PgSessionAsyncRunResult(PgsqlOpBuffer buffered_operations,
                          std::future<Status> future_status,
                          client::YBSessionPtr session);
  CHECKED_STATUS GetStatus(const PgSession& session);
  bool InProgress() const;

 private:
  // buffered_operations_ holds buffered operations (if any) which were applied to
  // the YBSession object before the very first non-bufferable operation.
  // Result of these operations will be checked in the GetStatus() method.
  PgsqlOpBuffer       buffered_operations_;
  std::future<Status> future_status_;
  client::YBSessionPtr session_;
};

struct PgForeignKeyReference {
  PgForeignKeyReference(PgOid table_id, std::string ybctid);
  PgOid table_id;
  std::string ybctid;
};

// Represents row id (ybctid) from the DocDB's point of view.
class RowIdentifier {
 public:
  explicit RowIdentifier(const client::YBPgsqlWriteOp& op);
  inline const string& ybctid() const;
  inline const string& table_id() const;

 private:
  const std::string* table_id_;
  const std::string* ybctid_;
  string             ybctid_holder_;
};

// This class is not thread-safe as it is mostly used by a single-threaded PostgreSQL backend
// process.
class PgSession : public RefCountedThreadSafe<PgSession> {
 public:
  // Public types.
  typedef scoped_refptr<PgSession> ScopedRefPtr;

  // Constructors.
  PgSession(client::YBClient* client,
            const string& database_name,
            scoped_refptr<PgTxnManager> pg_txn_manager,
            scoped_refptr<server::HybridClock> clock,
            const tserver::TServerSharedObject* tserver_shared_object,
            const YBCPgCallbacks& pg_callbacks);
  virtual ~PgSession();

  //------------------------------------------------------------------------------------------------
  // Operations on Session.
  //------------------------------------------------------------------------------------------------

  CHECKED_STATUS ConnectDatabase(const std::string& database_name);

  CHECKED_STATUS IsDatabaseColocated(const PgOid database_oid, bool *colocated);

  //------------------------------------------------------------------------------------------------
  // Operations on Database Objects.
  //------------------------------------------------------------------------------------------------

  // API for database operations.
  CHECKED_STATUS CreateDatabase(const std::string& database_name,
                                PgOid database_oid,
                                PgOid source_database_oid,
                                PgOid next_oid,
                                const boost::optional<TransactionMetadata> transaction,
                                const bool colocated);
  CHECKED_STATUS DropDatabase(const std::string& database_name, PgOid database_oid);
  client::YBNamespaceAlterer *NewNamespaceAlterer(const std::string& namespace_name,
                                                  PgOid database_oid);

  CHECKED_STATUS ReserveOids(PgOid database_oid,
                             PgOid nexte_oid,
                             uint32_t count,
                             PgOid *begin_oid,
                             PgOid *end_oid);

  CHECKED_STATUS GetCatalogMasterVersion(uint64_t *version);

  // API for sequences data operations.
  CHECKED_STATUS CreateSequencesDataTable();

  CHECKED_STATUS InsertSequenceTuple(int64_t db_oid,
                                     int64_t seq_oid,
                                     uint64_t ysql_catalog_version,
                                     int64_t last_val,
                                     bool is_called);

  CHECKED_STATUS UpdateSequenceTuple(int64_t db_oid,
                                     int64_t seq_oid,
                                     uint64_t ysql_catalog_version,
                                     int64_t last_val,
                                     bool is_called,
                                     boost::optional<int64_t> expected_last_val,
                                     boost::optional<bool> expected_is_called,
                                     bool* skipped);

  CHECKED_STATUS ReadSequenceTuple(int64_t db_oid,
                                   int64_t seq_oid,
                                   uint64_t ysql_catalog_version,
                                   int64_t *last_val,
                                   bool *is_called);

  CHECKED_STATUS DeleteSequenceTuple(int64_t db_oid, int64_t seq_oid);

  CHECKED_STATUS DeleteDBSequences(int64_t db_oid);

  //------------------------------------------------------------------------------------------------
  // Operations on Tablegroup.
  //------------------------------------------------------------------------------------------------

  CHECKED_STATUS CreateTablegroup(const std::string& database_name,
                                  const PgOid database_oid,
                                  PgOid tablegroup_oid);

  CHECKED_STATUS DropTablegroup(const PgOid database_oid,
                                PgOid tablegroup_oid);

  // API for schema operations.
  // TODO(neil) Schema should be a sub-database that have some specialized property.
  CHECKED_STATUS CreateSchema(const std::string& schema_name, bool if_not_exist);
  CHECKED_STATUS DropSchema(const std::string& schema_name, bool if_exist);

  // API for table operations.
  std::unique_ptr<client::YBTableCreator> NewTableCreator();
  std::unique_ptr<client::YBTableAlterer> NewTableAlterer(const client::YBTableName& table_name);
  std::unique_ptr<client::YBTableAlterer> NewTableAlterer(const string table_id);
  CHECKED_STATUS DropTable(const PgObjectId& table_id);
  CHECKED_STATUS DropIndex(
      const PgObjectId& index_id,
      client::YBTableName* indexed_table_name = nullptr,
      bool wait = true);
  CHECKED_STATUS TruncateTable(const PgObjectId& table_id);
  CHECKED_STATUS BackfillIndex(const PgObjectId& table_id);
  Result<PgTableDesc::ScopedRefPtr> LoadTable(const PgObjectId& table_id);
  void InvalidateTableCache(const PgObjectId& table_id);

  // Start operation buffering. Buffering must not be in progress.
  void StartOperationsBuffering();
  // Flush all pending buffered operation and stop further buffering.
  // Buffering must be in progress.
  CHECKED_STATUS StopOperationsBuffering();
  // Stop further buffering. Buffering may be in any state,
  // but pending buffered operations are not allowed.
  CHECKED_STATUS ResetOperationsBuffering();

  // Flush all pending buffered operations. Buffering mode remain unchanged.
  CHECKED_STATUS FlushBufferedOperations();
  // Drop all pending buffered operations. Buffering mode remain unchanged.
  void DropBufferedOperations();

  // Run (apply + flush) the given operation to read and write database content.
  // Template is used here to handle all kind of derived operations
  // (shared_ptr<YBPgsqlReadOp>, shared_ptr<YBPgsqlWriteOp>)
  // without implicitly conversion to shared_ptr<YBPgsqlReadOp>.
  // Conversion to shared_ptr<YBPgsqlOp> will be done later and result will re-used with move.
  template<class Op>
  Result<PgSessionAsyncRunResult> RunAsync(const std::shared_ptr<Op>& op,
                                           const PgObjectId& relation_id,
                                           uint64_t* read_time,
                                           bool force_non_bufferable) {
    return RunAsync(&op, 1, relation_id, read_time, force_non_bufferable);
  }

  // Run (apply + flush) list of given operations to read and write database content.
  template<class Op>
  Result<PgSessionAsyncRunResult> RunAsync(const std::vector<std::shared_ptr<Op>>& ops,
                                           const PgObjectId& relation_id,
                                           uint64_t* read_time,
                                           bool force_non_bufferable) {
    return RunAsync(ops.data(), ops.size(), relation_id, read_time, force_non_bufferable);
  }

  // Run multiple operations.
  template<class Op>
  Result<PgSessionAsyncRunResult> RunAsync(const std::shared_ptr<Op>* op,
                                           size_t ops_count,
                                           const PgObjectId& relation_id,
                                           uint64_t* read_time,
                                           bool force_non_bufferable) {
    SCHECK_GT(ops_count, 0, IllegalState, "Operation list must not be empty");
    RunHelper runner(relation_id, this, ShouldHandleTransactionally(**op));
    for (auto end = op + ops_count; op != end; ++op) {
      RETURN_NOT_OK(runner.Apply(*op, read_time, force_non_bufferable));
    }
    return runner.Flush();
  }

  //------------------------------------------------------------------------------------------------
  // Access functions.
  // TODO(neil) Need to double check these code later.
  // - This code in CQL processor has a lock. CQL comment: It can be accessed by multiple calls in
  //   parallel so they need to be thread-safe for shared reads / exclusive writes.
  //
  // - Currently, for each session, server executes the client requests sequentially, so the
  //   the following mutex is not necessary. I don't think we're capable of parallel-processing
  //   multiple statements within one session.
  //
  // TODO(neil) MUST ADD A LOCK FOR ACCESSING AND MODIFYING DATABASE BECAUSE WE USE THIS VARIABLE
  // AS INDICATOR FOR ALIVE OR DEAD SESSIONS.

  // Access functions for connected database.
  const char* connected_dbname() const {
    return connected_database_.c_str();
  }

  const string& connected_database() const {
    return connected_database_;
  }
  void set_connected_database(const std::string& database) {
    connected_database_ = database;
  }
  void reset_connected_database() {
    connected_database_ = "";
  }

  // Generate a new random and unique rowid. It is a v4 UUID.
  string GenerateNewRowid() {
    return rowid_generator_.Next(true /* binary_id */);
  }

  void InvalidateCache() {
    table_cache_.clear();
  }

  void InvalidateForeignKeyReferenceCache() {
    fk_reference_cache_.clear();
    fk_reference_intent_.clear();
  }

  // Check if initdb has already been run before. Needed to make initdb idempotent.
  Result<bool> IsInitDbDone();

  // Return the local tserver's catalog version stored in shared memory or an error if the shared
  // memory has not been initialized (e.g. in initdb).
  Result<uint64_t> GetSharedCatalogVersion();
  // Return the local tserver's postgres authentication key stored in shared memory or an error if
  // the shared memory has not been initialized (e.g. in initdb).
  Result<uint64_t> GetSharedAuthKey();

  using YbctidReader =
      std::function<Result<std::vector<std::string>>(PgOid, const std::vector<Slice>&)>;
  Result<bool> ForeignKeyReferenceExists(
      PgOid table_id, const Slice& ybctid, const YbctidReader& reader);
  void AddForeignKeyReferenceIntent(PgOid table_id, const Slice& ybctid);

  // Deletes the row referenced by ybctid from FK reference cache.
  void DeleteForeignKeyReference(PgOid table_id, const Slice& ybctid);

  CHECKED_STATUS HandleResponse(const client::YBPgsqlOp& op, const PgObjectId& relation_id) const;

  CHECKED_STATUS TabletServerCount(int *tserver_count, bool primary_only = false,
      bool use_cache = false);

  // Sets the specified timeout in the rpc service.
  void SetTimeout(int timeout_ms);

  Result<IndexPermissions> WaitUntilIndexPermissionsAtLeast(
      const PgObjectId& table_id,
      const PgObjectId& index_id,
      const IndexPermissions& target_index_permissions);

  CHECKED_STATUS AsyncUpdateIndexPermissions(const PgObjectId& indexed_table_id);

 private:
  using Flusher = std::function<Status(PgsqlOpBuffer, bool)>;

  CHECKED_STATUS FlushBufferedOperationsImpl(const Flusher& flusher);
  CHECKED_STATUS FlushOperations(PgsqlOpBuffer ops, bool transactional);
  CHECKED_STATUS ApplyOperation(client::YBSession* session,
                                bool transactional,
                                const BufferableOperation& bop);

  // Helper class to run multiple operations on single session.
  // This class allows to keep implementation of RunAsync template method simple
  // without moving its implementation details into header file.
  class RunHelper {
   public:
    RunHelper(const PgObjectId& relation_id, PgSession* pg_session, bool transactional);
    CHECKED_STATUS Apply(std::shared_ptr<client::YBPgsqlOp> op,
                         uint64_t* read_time,
                         bool force_non_bufferable);
    Result<PgSessionAsyncRunResult> Flush();

   private:
    const PgObjectId& relation_id_;
    PgSession& pg_session_;
    const bool transactional_;
    PgsqlOpBuffer& buffer_;
    // pending_ops_ holds previously buffered operations which were applied to YBSession
    // before the very first non-bufferable operation. Result of these operations will be checked
    // by the PgSessionAsyncRunResult object returned from the Flush() method.
    PgsqlOpBuffer pending_ops_;
    client::YBSessionPtr yb_session_;
  };

  // Returns the appropriate session to use, in most cases the one used by the current transaction.
  // read_only_op - whether this is being done in the context of a read-only operation. For
  //                non-read-only operations we make sure to start a YB transaction.
  // We are returning a raw pointer here because the returned session is owned either by the
  // PgTxnManager or by this object.
  Result<client::YBSession*> GetSession(bool transactional,
                                        bool read_only_op,
                                        bool needs_pessimistic_locking = false);

  // Flush buffered write operations from the given buffer.
  Status FlushBufferedWriteOperations(PgsqlOpBuffer* write_ops, bool transactional);

  // Whether we should use transactional or non-transactional session.
  bool ShouldHandleTransactionally(const client::YBPgsqlOp& op);

  // YBClient, an API that SQL engine uses to communicate with all servers.
  client::YBClient* const client_;

  // YBSession to execute operations.
  std::shared_ptr<client::YBSession> session_;

  // Connected database.
  std::string connected_database_;

  // A transaction manager allowing to begin/abort/commit transactions.
  scoped_refptr<PgTxnManager> pg_txn_manager_;

  const scoped_refptr<server::HybridClock> clock_;

  // Execution status.
  Status status_;
  string errmsg_;

  // Rowid generator.
  ObjectIdGenerator rowid_generator_;

  std::unordered_map<TableId, std::shared_ptr<client::YBTable>> table_cache_;
  boost::unordered_set<PgForeignKeyReference> fk_reference_cache_;
  boost::unordered_set<PgForeignKeyReference> fk_reference_intent_;

  // Should write operations be buffered?
  bool buffering_enabled_ = false;
  PgsqlOpBuffer buffered_ops_;
  PgsqlOpBuffer buffered_txn_ops_;
  std::unordered_set<RowIdentifier, boost::hash<RowIdentifier>> buffered_keys_;

  const tserver::TServerSharedObject* const tserver_shared_object_;
  const YBCPgCallbacks& pg_callbacks_;

  bool read_from_followers_ = false;
};

}  // namespace pggate
}  // namespace yb

#endif // YB_YQL_PGGATE_PG_SESSION_H_
