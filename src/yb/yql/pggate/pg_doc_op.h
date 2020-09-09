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

#ifndef YB_YQL_PGGATE_PG_DOC_OP_H_
#define YB_YQL_PGGATE_PG_DOC_OP_H_

#include <deque>

#include <boost/optional.hpp>

#include "yb/util/locks.h"
#include "yb/client/yb_op.h"
#include "yb/yql/pggate/pg_session.h"

namespace yb {
namespace pggate {

YB_STRONGLY_TYPED_BOOL(RequestSent);

//--------------------------------------------------------------------------------------------------
// PgDocResult represents a batch of rows in ONE reply from tablet servers.
class PgDocResult {
 public:
  explicit PgDocResult(string&& data);
  PgDocResult(string&& data, std::list<int64_t>&& row_orders);
  ~PgDocResult();

  PgDocResult(const PgDocResult&) = delete;
  PgDocResult& operator=(const PgDocResult&) = delete;

  // Get the order of the next row in this batch.
  int64_t NextRowOrder();

  // End of this batch.
  bool is_eof() const {
    return row_count_ == 0 || row_iterator_.empty();
  }

  // Get the postgres tuple from this batch.
  CHECKED_STATUS WritePgTuple(const std::vector<PgExpr*>& targets, PgTuple *pg_tuple,
                              int64_t *row_order);

  // Get system columns' values from this batch.
  // Currently, we only have ybctids, but there could be more.
  CHECKED_STATUS ProcessSystemColumns();

  // Access function to ybctids value in this batch.
  // Sys columns must be processed before this function is called.
  const vector<Slice>& ybctids() const {
    DCHECK(syscol_processed_) << "System columns are not yet setup";
    return ybctids_;
  }

  // Row count in this batch.
  int64_t row_count() const {
    return row_count_;
  }

 private:
  // Data selected from DocDB.
  string data_;

  // Iterator on "data_" from row to row.
  Slice row_iterator_;

  // The row number of only this batch.
  int64_t row_count_ = 0;

  // The indexing order of the row in this batch.
  // These order values help to identify the row order across all batches.
  std::list<int64_t> row_orders_;

  // System columns.
  // - ybctids_ contains pointers to the buffers "data_".
  // - System columns must be processed before these fields have any meaning.
  vector<Slice> ybctids_;
  bool syscol_processed_ = false;
};

//--------------------------------------------------------------------------------------------------
// Doc operation API
// Classes
// - PgDocOp: Shared functionalities among all ops, mostly just RPC calls to tablet servers.
// - PgDocReadOp: Definition for data & method members to be used in READ operation.
// - PgDocWriteOp: Definition for data & method members to be used in READ operation.
// - PgDocResult: Definition data holder before they are passed to Postgres layer.
//
// Processing Steps
// (1) Collecting Data:
//     PgGate collects data from Posgres and write to a "PgDocOp::Template".
//
// (2) Create operators:
//     When no optimization is applied, the "template_op" is executed as is. When an optimization
//     is chosen, PgDocOp will clone the template to populate operators and kept them in vector
//     "pgsql_ops_". When an op executes arguments, it sends request and reads replies from servers.
//
//     * Vector "pgsql_ops_" is of fixed size for the entire execution, and its contents (YBPgsqlOp
//       shared_ptrs) also remain for the entire execution.
//     * There is a LIMIT on how many pgsql-op can be cloned. If the number of requests / arguments
//       are higher than the LIMIT, some requests will have to wait in queue until the execution
//       of precedent arguments are completed.
//     * After an argument input is executed, its associated YBPgsqlOp will be reused to execute
//       a new set of arguments. We don't clone new ones for new arguments.
//     * When a YBPgsqlOp is reused, its YBPgsqlOp::ProtobufRequest will be updated appropriately
//       with new arguments.
//     * NOTE: Some operators in "pgsql_ops_" might not be active (no arguments) at a given time
//       of execution. For example, some ops might complete their execution while others have
//       paging state and are sent again to table server.
//
// (3) SendRequest:
//     PgSession API requires contiguous array of operators. For this reason, before sending the
//     pgsql_ops_ is soreted to place active ops first, and all inactive ops are place at the end.
//     For example,
//        PgSession::RunAsync(pgsql_ops_.data(), active_op_count)
//
// (4) ReadResponse:
//     Response are written to a local cache PgDocResult.
//
// This API has several sets of methods and attributes for different purposes.
// (1) Build request.
//  This section collect information and data from PgGate API.
//  * Attributes
//    - relation_id_: Table to be operated on.
//    - template_op_ of type YBPgsqlReadOp and YBPgsqlWriteOp.
//      This object contains statement descriptions and expression values from users.
//      All user-provided arguments are kept in this attributes.
//  * Methods
//    - Class constructors.
//
// (2) Constructing protobuf request.
//  This section populates protobuf requests using the collected information in "template_op_".
//  - Without optimization, the protobuf request in "template_op_" will be used .
//  - With parallel optimization, multiple protobufs are constructed by cloning template into many
//    operators. How the execution are subdivided is depending on the parallelism method.
//  NOTE Whenever we support PREPARE(stmt), we'd stop processing at after this step for PREPARE.
//
//  * Attributes
//    - YBPgsqlOp pgsql_ops_: Contains all protobuf requests to be sent to tablet servers.
//  * Methods
//    - When there isn't any optimization, template_op_ is used.
//        pgsql_ops_[0] = template_op_
//    - CreateRequests()
//    - ClonePgsqlOps() Clone template_op_ into one or more ops.
//    - PopulateParallelSelectCountOps() Parallel processing SELECT COUNT.
//      The same requests are constructed for each tablet server.
//    - PopulateNextHashPermutationOps() Parallel processing SELECT by hash conditions.
//      Hash permutations will be group into different request based on their hash_codes.
//    - PopulateDmlByYbctidOps() Parallel processing SELECT by ybctid values.
//      Ybctid values will be group into different request based on their hash_codes.
//      This function is a bit different from other formulating function because it is used for an
//      internal request within PgGate. Other populate functions are used for external requests
//      from Postgres layer via PgGate API.
//
// (3) Execution
//  This section exchanges RPC calls with tablet servers.
//  * Attributes
//    - active_op_counts_: Number of active operators in vector "pgsql_ops_".
//        Exec/active op range = pgsql_ops_[0, active_op_count_)
//        Inactive op range = pgsql_ops_[active_op_count_, total_count)
//      The vector pgsql_ops_ is fixed sized, can have inactive operators as operators are not
//      completing execution at the same time.
//  * Methods
//    - ExecuteInit()
//    - Execute() Driver for all RPC related effort.
//    - SendRequest() Send request for active operators to tablet server using YBPgsqlOp.
//        RunAsync(pgsql_ops_.data(), active_op_count_)
//    - ProcessResponse() Get response from tablet server using YBPgsqlOp.
//    - MoveInactiveOpsOutside() Sort pgsql_ops_ to move inactive operators outside of exec range.
//
// (4) Return result
//  This section return result via PgGate API to postgres.
//  * Attributes
//    - Objects of class PgDocResult
//    - rows_affected_count_: Number of rows that was operated by this doc_op.
//  * Methods
//    - GetResult()
//    - GetRowsAffectedCount()
//
// TODO(dmitry / neil) Allow sending active requests and receive their response one at a time.
//
// To process data in parallel, the operators must be able to run independently from one another.
// However, currently operators are executed in batches and together even though they belong to
// different partitions and interact with different tablet servers.
//--------------------------------------------------------------------------------------------------

class PgDocOp : public std::enable_shared_from_this<PgDocOp> {
 public:
  // Public types.
  typedef std::shared_ptr<PgDocOp> SharedPtr;
  typedef std::shared_ptr<const PgDocOp> SharedPtrConst;

  typedef std::unique_ptr<PgDocOp> UniPtr;
  typedef std::unique_ptr<const PgDocOp> UniPtrConst;

  // Constructors & Destructors.
  explicit PgDocOp(const PgSession::ScopedRefPtr& pg_session,
                   const PgTableDesc::ScopedRefPtr& table_desc,
                   const PgObjectId& relation_id = PgObjectId());
  virtual ~PgDocOp();

  // Initialize doc operator.
  virtual void ExecuteInit(const PgExecParameters *exec_params);

  // Execute the op. Return true if the request has been sent and is awaiting the result.
  virtual Result<RequestSent> Execute(bool force_non_bufferable = false);

  // Instruct this doc_op to abandon execution and querying data by setting end_of_data_ to 'true'.
  // - This op will not send request to tablet server.
  // - This op will return empty result-set when being requested for data.
  void AbandonExecution() {
    end_of_data_ = true;
  }

  // Get the result of the op. No rows will be added to rowsets in case end of data reached.
  CHECKED_STATUS GetResult(std::list<PgDocResult> *rowsets);
  Result<int32_t> GetRowsAffectedCount() const;

  // This operation is requested internally within PgGate, and that request does not go through
  // all the steps as other operation from Postgres thru PgDocOp. This is used to create requests
  // for the following select.
  //   SELECT ... FROM <table> WHERE ybctid IN (SELECT base_ybctids from INDEX)
  // After ybctids are queried from INDEX, PgGate will call "PopulateDmlByYbctidOps" to create
  // operators to fetch rows whose rowids equal queried ybctids.
  virtual CHECKED_STATUS PopulateDmlByYbctidOps(const vector<Slice> *ybctids) = 0;

 protected:
  // Populate Protobuf requests using the collected informtion for this DocDB operator.
  virtual CHECKED_STATUS CreateRequests() = 0;

  // Create operators.
  // - Each operator is used for one request.
  // - When parallelism by partition is applied, each operator is associated with one partition,
  //   and each operator has a batch of arguments that belong to that partition.
  //   * The higher the number of partition_count, the higher the parallelism level.
  //   * If (partition_count == 1), only one operator is needed for the entire partition range.
  //   * If (partition_count > 1), each operator is used for a specific partition range.
  //   * This optimization is used by
  //       PopulateDmlByYbctidOps()
  //       PopulateParallelSelectCountOps()
  // - When parallelism by arguments is applied, each operator has only one argument.
  //   When tablet server will run the requests in parallel as it assigned one thread per request.
  //       PopulateNextHashPermutationOps()
  CHECKED_STATUS ClonePgsqlOps(int op_count);

  // Only active operators are kept in the active range [0, active_op_count_)
  // - Not execute operators that are outside of range [0, active_op_count_).
  // - Sort the operators in "pgsql_ops_" to move "inactive" operators to the end of the list.
  void MoveInactiveOpsOutside();

  // Clone READ or WRITE "template_op_" into new operators.
  virtual std::unique_ptr<client::YBPgsqlOp> CloneFromTemplate() = 0;

  // Process the result set in server response.
  Result<std::list<PgDocResult>> ProcessResponseResult();

  void SetReadTime();

 private:
  CHECKED_STATUS SendRequest(bool force_non_bufferable);

  virtual CHECKED_STATUS SendRequestImpl(bool force_non_bufferable);

  Result<std::list<PgDocResult>> ProcessResponse(const Status& exec_status);

  virtual Result<std::list<PgDocResult>> ProcessResponseImpl() = 0;

  //----------------------------------- Data Members -----------------------------------------------
 protected:
  // Session control.
  PgSession::ScopedRefPtr pg_session_;

  // Operation time. This time is set at the start and must stay the same for the lifetime of the
  // operation to ensure that it is operating on one snapshot.
  uint64_t read_time_ = 0;

  // Target table.
  PgTableDesc::ScopedRefPtr table_desc_;
  PgObjectId relation_id_;

  // Exec control parameters.
  PgExecParameters exec_params_;

  // Suppress sending new request after processing response.
  // Next request will be sent in case upper level will ask for additional data.
  bool suppress_next_result_prefetching_ = false;

  // Populated protobuf request.
  std::vector<std::shared_ptr<client::YBPgsqlOp>> pgsql_ops_;

  // Number of active operators in the pgsql_ops_ list.
  int32_t active_op_count_ = 0;

  // Indicator for completing all request populations.
  bool request_population_completed_ = false;

  // If true, all data for each batch must be collected before PgGate gets the reply.
  // NOTE:
  // - Currently, PgSession's default behavior is to get all responses in a batch together.
  // - We set this flag only to prevent future optimization where requests & their responses to
  //   and from different tablet servers are sent and received independently. That optimization
  //   should only be done when "wait_for_batch_completion_ == false"
  bool wait_for_batch_completion_ = true;

  // Future object to fetch a response from DocDB after sending a request.
  // Object's valid() method returns false in case no request is sent
  // or sent request was buffered by the session.
  // Only one RunAsync() can be called to sent to DocDB at a time.
  PgSessionAsyncRunResult response_;

  // Executed row count.
  int32_t rows_affected_count_ = 0;

  // Whether all requested data by the statement has been received or there's a run-time error.
  bool end_of_data_ = false;

  // The order number of each request when batching arguments.
  // Currently, this is used for query by YBCTID.
  // - Each pgsql_op has a batch of ybctids selected from INDEX.
  // - The order of resulting rows should match with the order of queried ybctids.
  // - Example:
  //   Suppose we got from INDEX table
  //     { ybctid_1, ybctid_2, ybctid_3, ybctid_4, ybctid_5, ybctid_6, ybctid_7 }
  //
  //   Now pgsql_op are constructed as the following, one op per partition.
  //     pgsql_op <partition 1> (ybctid_1, ybctid_3, ybctid_4)
  //     pgsql_op <partition 2> (ybctid_2, ybctid_6)
  //     pgsql_op <partition 2> (ybctid_5, ybctid_7)
  //
  //   After getting the rows of data from pgsql, the rows must be then ordered from 1 thru 7.
  //   To do so, for each pgsql_op we kept an array of orders, batch_row_orders_.
  //   For the above pgsql_ops_, the orders would be cached as the following.
  //     vector orders { partition 1: list ( 1, 3, 4 ),
  //                     partition 2: list ( 2, 6 ),
  //                     partition 3: list ( 5, 7 ) }
  //
  //   When the "pgsql_ops_" elements are sorted and swapped order, the "batch_row_orders_"
  //   must be swaped also.
  //     std::swap ( pgsql_ops_[1], pgsql_ops_[3])
  //     std::swap ( batch_row_orders_[1], batch_row_orders_[3] )
  std::vector<std::list<int64_t>> batch_row_orders_;

  // This counter is used to maintain the row order when the operator sends requests in parallel
  // by partition. Currently only query by YBCTID uses this variable.
  int64_t batch_row_ordering_counter_ = 0;

  // Parallelism level.
  // - This is the maximum number of read/write requests being sent to servers at one time.
  // - When it is 1, there's no optimization. Available requests is executed one at a time.
  int32_t parallelism_level_ = 1;

 private:
  // Result set either from selected or returned targets is cached in a list of strings.
  // Querying state variables.
  Status exec_status_ = Status::OK();
};

//--------------------------------------------------------------------------------------------------

class PgDocReadOp : public PgDocOp {
 public:
  // Public types.
  typedef std::shared_ptr<PgDocReadOp> SharedPtr;
  typedef std::shared_ptr<const PgDocReadOp> SharedPtrConst;

  typedef std::unique_ptr<PgDocReadOp> UniPtr;
  typedef std::unique_ptr<const PgDocReadOp> UniPtrConst;

  // Constructors & Destructors.
  PgDocReadOp(const PgSession::ScopedRefPtr& pg_session,
              const PgTableDesc::ScopedRefPtr& table_desc,
              std::unique_ptr<client::YBPgsqlReadOp> read_op);

  void ExecuteInit(const PgExecParameters *exec_params) override;

 private:
  // Create protobuf requests using template_op_.
  CHECKED_STATUS CreateRequests() override;

  // Create operators by partition.
  // - Optimization for statement
  //     SELECT xxx FROM <table> WHERE ybctid IN (SELECT ybctid FROM INDEX)
  // - After being queried from inner select, ybctids are used for populate request for outer query.
  CHECKED_STATUS PopulateDmlByYbctidOps(const vector<Slice> *ybctids) override;
  CHECKED_STATUS InitializeYbctidOperators();

  // Create operators by partition arguments.
  // - Optimization for statement:
  //     SELECT ... WHERE <hash-columns> IN <value-lists>
  // - If partition column binds are defined, partition_column_values field of each operation
  //   is set to be the next permutation.
  // - When an operator is assigned a hash permutation, it is marked as active to be executed.
  // - When an operator completes the execution, it is marked as inactive and available for the
  //   exection of the next hash permutation.
  CHECKED_STATUS PopulateNextHashPermutationOps();
  CHECKED_STATUS InitializeHashPermutationStates();

  // Create operators by partitions.
  // - Optimization for statement:
  //     Create parallel request for SELECT COUNT().
  CHECKED_STATUS PopulateParallelSelectCountOps();

  // Set partition boundaries to a given partition.
  CHECKED_STATUS SetScanPartitionBoundary();

  // Process response from DocDB.
  Result<std::list<PgDocResult>> ProcessResponseImpl() override;

  // Process response paging state from DocDB.
  CHECKED_STATUS ProcessResponsePagingState();

  // Reset pgsql operators before reusing them with new arguments / inputs from Postgres.
  CHECKED_STATUS ResetInactivePgsqlOps();

  // Analyze options and pick the appropriate prefetch limit.
  void SetRequestPrefetchLimit();

  // Set the row_mark_type field of our read request based on our exec control parameter.
  void SetRowMark();

  // Set the read_time for our read request based on our exec control parameter.
  void SetReadTime();

  // Clone the template into actual requests to be sent to server.
  std::unique_ptr<client::YBPgsqlOp> CloneFromTemplate() override {
    return template_op_->DeepCopy();
  }

  // Get the read_op for a specific operation index from pgsql_ops_.
  client::YBPgsqlReadOp *GetReadOp(int op_index) {
    return static_cast<client::YBPgsqlReadOp *>(pgsql_ops_[op_index].get());
  }

  // Re-format the request when connecting to older server during rolling upgrade.
  void FormulateRequestForRollingUpgrade(PgsqlReadRequestPB *read_req);

  //----------------------------------- Data Members -----------------------------------------------

  // Template operation, used to fill in pgsql_ops_ by either assigning or cloning.
  std::shared_ptr<client::YBPgsqlReadOp> template_op_;

  // Used internally for PopulateNextHashPermutationOps to keep track of which permutation should
  // be used to construct the next read_op.
  // Is valid as long as request_population_completed_ is false.
  //
  // Example:
  // For a query clause "h1 = 1 AND h2 IN (2,3) AND h3 IN (4,5,6) AND h4 = 7",
  // there are 1*2*3*1 = 6 possible permutation.
  // As such, this field will take on values 0 through 5.
  int total_permutation_count_ = 0;
  int next_permutation_idx_ = 0;

  // Used internally for PopulateNextHashPermutationOps to holds all partition expressions.
  // Elements correspond to a hash columns, in the same order as they were defined
  // in CREATE TABLE statement.
  // This is somewhat similar to what hash_values_options_ in CQL is used for.
  //
  // Example:
  // For a query clause "h1 = 1 AND h2 IN (2,3) AND h3 IN (4,5,6) AND h4 = 7",
  // this will be initialized to [[1], [2, 3], [4, 5, 6], [7]]
  std::vector<std::vector<const PgsqlExpressionPB*>> partition_exprs_;
};

//--------------------------------------------------------------------------------------------------

class PgDocWriteOp : public PgDocOp {
 public:
  // Public types.
  typedef std::shared_ptr<PgDocWriteOp> SharedPtr;
  typedef std::shared_ptr<const PgDocWriteOp> SharedPtrConst;

  typedef std::unique_ptr<PgDocWriteOp> UniPtr;
  typedef std::unique_ptr<const PgDocWriteOp> UniPtrConst;

  // Constructors & Destructors.
  PgDocWriteOp(const PgSession::ScopedRefPtr& pg_session,
               const PgTableDesc::ScopedRefPtr& table_desc,
               const PgObjectId& relation_id,
               std::unique_ptr<client::YBPgsqlWriteOp> write_op);

  // Set write time.
  void SetWriteTime(const HybridTime& write_time);

 private:
  // Process response implementation.
  Result<std::list<PgDocResult>> ProcessResponseImpl() override;

  // Create protobuf requests using template_op (write_op).
  CHECKED_STATUS CreateRequests() override;

  // For write ops, we are not yet batching ybctid from index query.
  // TODO(neil) This function will be implemented when we push down sub-query inside WRITE ops to
  // the proxy layer. There's many scenarios where this optimization can be done.
  CHECKED_STATUS PopulateDmlByYbctidOps(const vector<Slice> *ybctids) override {
    LOG(FATAL) << "Not yet implemented";
    return Status::OK();
  }

  // Get WRITE operator for a specific operator index in pgsql_ops_.
  client::YBPgsqlWriteOp *GetWriteOp(int op_index) {
    return static_cast<client::YBPgsqlWriteOp *>(pgsql_ops_[op_index].get());
  }

  // Clone user data from template to actual protobuf requests.
  std::unique_ptr<client::YBPgsqlOp> CloneFromTemplate() override {
    return write_op_->DeepCopy();
  }

  //----------------------------------- Data Members -----------------------------------------------
  // Template operation all write ops.
  std::shared_ptr<client::YBPgsqlWriteOp> write_op_;
};

}  // namespace pggate
}  // namespace yb

#endif // YB_YQL_PGGATE_PG_DOC_OP_H_
