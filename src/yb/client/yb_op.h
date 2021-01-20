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
#ifndef YB_CLIENT_YB_OP_H_
#define YB_CLIENT_YB_OP_H_

#include <memory>
#include <string>

#include <boost/optional.hpp>

#include "yb/client/client_fwd.h"

#include "yb/common/partial_row.h"
#include "yb/common/partition.h"
#include "yb/common/read_hybrid_time.h"

namespace yb {

class RedisWriteRequestPB;
class RedisReadRequestPB;
class RedisResponsePB;

class QLWriteRequestPB;
class QLReadRequestPB;
class QLResponsePB;
class QLRowBlock;

namespace client {

namespace internal {
class Batcher;
class AsyncRpc;
class RemoteTablet;
}  // namespace internal

class YBSession;
class YBStatusCallback;
class YBTable;

YB_DEFINE_ENUM(OpGroup, (kWrite)(kLeaderRead)(kConsistentPrefixRead));

// A write or read operation operates on a single table and partial row.
// The YBOperation class itself allows the batcher to get to the
// generic information that it needs to process all write operations.
//
// On its own, the class does not represent any specific change and thus cannot
// be constructed independently.
//
// YBOperation also holds shared ownership of its YBTable to allow client's
// scope to end while the YBOperation is still alive.
class YBOperation {
 public:
  enum Type {
    // Redis opcodes.
    REDIS_WRITE = 4,
    REDIS_READ = 5,

    // CQL opcodes.
    QL_WRITE = 6,
    QL_READ = 7,

    // Postgresql opcodes.
    PGSQL_WRITE = 8,
    PGSQL_READ = 9,
  };
  virtual ~YBOperation();

  std::shared_ptr<const YBTable> table() const { return table_; }

  void ResetTable(std::shared_ptr<YBTable> new_table);

  virtual std::string ToString() const = 0;
  virtual Type type() const = 0;
  virtual bool read_only() const = 0;
  virtual bool succeeded() const = 0;
  virtual bool returns_sidecar() = 0;

  virtual OpGroup group() {
    return read_only() ? OpGroup::kLeaderRead : OpGroup::kWrite;
  }

  virtual bool applied() {
    return succeeded();
  }

  virtual bool should_add_intents(IsolationLevel isolation_level) {
    return !read_only() || isolation_level == IsolationLevel::SERIALIZABLE_ISOLATION;
  }

  virtual void SetHashCode(uint16_t hash_code) = 0;

  const scoped_refptr<internal::RemoteTablet>& tablet() const {
    return tablet_;
  }

  void SetTablet(const scoped_refptr<internal::RemoteTablet>& tablet);

  // Returns the partition key of the operation.
  virtual CHECKED_STATUS GetPartitionKey(std::string* partition_key) const = 0;

  // Returns whether this operation is being performed on a table where distributed transactions
  // are enabled.
  virtual bool IsTransactional() const;

  // Whether this is an operation on one of the YSQL system catalog tables.
  bool IsYsqlCatalogOp() const;

  // Mark table this op is designated for as having stale partitions.
  void MarkTablePartitionsAsStale();

  // Refreshes partitions of the table this op is designated of in case partitions have been marked
  // as stale.
  // Returns whether table partitions have been refreshed.
  Result<bool> MaybeRefreshTablePartitions();

  int64_t GetQueryId() const {
    return reinterpret_cast<int64_t>(this);
  }

 protected:
  explicit YBOperation(const std::shared_ptr<YBTable>& table);

  std::shared_ptr<YBTable> table_;

 private:
  friend class internal::AsyncRpc;

  scoped_refptr<internal::RemoteTablet> tablet_;

  DISALLOW_COPY_AND_ASSIGN(YBOperation);
};

//--------------------------------------------------------------------------------------------------
// YBRedis Operators.
//--------------------------------------------------------------------------------------------------

class YBRedisOp : public YBOperation {
 public:
  explicit YBRedisOp(const std::shared_ptr<YBTable>& table);

  bool has_response() { return redis_response_ ? true : false; }
  virtual size_t space_used_by_request() const = 0;

  const RedisResponsePB& response() const;

  RedisResponsePB* mutable_response();

  uint16_t hash_code() const { return hash_code_; }

  // Redis does not use sidecars.
  bool returns_sidecar() override { return false; }

  virtual const std::string& GetKey() const = 0;

 protected:
  uint16_t hash_code_ = 0;
  std::unique_ptr<RedisResponsePB> redis_response_;
};

class YBRedisWriteOp : public YBRedisOp {
 public:
  explicit YBRedisWriteOp(const std::shared_ptr<YBTable>& table);

  // Note: to avoid memory copy, this RedisWriteRequestPB is moved into tserver WriteRequestPB
  // when the request is sent to tserver. It is restored after response is received from tserver
  // (see WriteRpc's constructor).
  const RedisWriteRequestPB& request() const { return *redis_write_request_; }
  size_t space_used_by_request() const override;

  RedisWriteRequestPB* mutable_request() { return redis_write_request_.get(); }

  std::string ToString() const override;

  bool read_only() const override { return false; }
  bool succeeded() const override { return false; } // TODO(dtxn) implement

  // Set the hash key in the WriteRequestPB.
  void SetHashCode(uint16_t hash_code) override;

  const std::string& GetKey() const override;

  CHECKED_STATUS GetPartitionKey(std::string* partition_key) const override;

 protected:
  virtual Type type() const override { return REDIS_WRITE; }

 private:
  friend class YBTable;
  std::unique_ptr<RedisWriteRequestPB> redis_write_request_;
  std::unique_ptr<RedisResponsePB> redis_response_;
};


class YBRedisReadOp : public YBRedisOp {
 public:
  explicit YBRedisReadOp(const std::shared_ptr<YBTable>& table);

  // Note: to avoid memory copy, this RedisReadRequestPB is moved into tserver ReadRequestPB
  // when the request is sent to tserver. It is restored after response is received from tserver
  // (see ReadRpc's constructor).
  const RedisReadRequestPB& request() const { return *redis_read_request_; }
  size_t space_used_by_request() const override;

  RedisReadRequestPB* mutable_request() { return redis_read_request_.get(); }

  std::string ToString() const override;

  bool read_only() const override { return true; }
  bool succeeded() const override { return false; } // TODO(dtxn) implement

  // Set the hash key in the ReadRequestPB.
  void SetHashCode(uint16_t hash_code) override;

  const std::string& GetKey() const override;

  CHECKED_STATUS GetPartitionKey(std::string* partition_key) const override;

 protected:
  Type type() const override { return REDIS_READ; }
  OpGroup group() override;

 private:
  friend class YBTable;
  std::unique_ptr<RedisReadRequestPB> redis_read_request_;
};

//--------------------------------------------------------------------------------------------------
// YBCql Operators.
//--------------------------------------------------------------------------------------------------

class YBqlOp : public YBOperation {
 public:
  ~YBqlOp();

  const QLResponsePB& response() const { return *ql_response_; }

  QLResponsePB* mutable_response() { return ql_response_.get(); }

  const std::string& rows_data() { return rows_data_; }

  std::string* mutable_rows_data() { return &rows_data_; }

  bool succeeded() const override { return response().status() == QLResponsePB::YQL_STATUS_OK; }

 protected:
  explicit YBqlOp(const std::shared_ptr<YBTable>& table);
  std::unique_ptr<QLResponsePB> ql_response_;
  std::string rows_data_;
};

class YBqlWriteOp : public YBqlOp {
 public:
  explicit YBqlWriteOp(const std::shared_ptr<YBTable>& table);
  ~YBqlWriteOp();

  // Note: to avoid memory copy, this QLWriteRequestPB is moved into tserver WriteRequestPB
  // when the request is sent to tserver. It is restored after response is received from tserver
  // (see WriteRpc's constructor).
  const QLWriteRequestPB& request() const { return *ql_write_request_; }

  QLWriteRequestPB* mutable_request() { return ql_write_request_.get(); }

  std::string ToString() const override;

  bool read_only() const override { return false; };

  bool returns_sidecar() override {
    return ql_write_request_->has_if_expr() || ql_write_request_->returns_status();
  }

  void SetHashCode(uint16_t hash_code) override;

  uint16_t GetHashCode() const;

  CHECKED_STATUS GetPartitionKey(std::string* partition_key) const override;

  // Hash and equal functions to define a set of write operations that do not overlap by their
  // hash (or primary) keys.
  struct HashKeyComparator {
    virtual ~HashKeyComparator() {}
    virtual size_t operator() (const YBqlWriteOpPtr& op) const;
    virtual bool operator() (const YBqlWriteOpPtr& op1, const YBqlWriteOpPtr& op2) const;
  };
  struct PrimaryKeyComparator : HashKeyComparator {
    virtual ~PrimaryKeyComparator() {}
    size_t operator() (const YBqlWriteOpPtr& op) const override;
    bool operator() (const YBqlWriteOpPtr& op1, const YBqlWriteOpPtr& op2) const override;
  };

  // Does this operation read/write the static or primary row?
  bool ReadsStaticRow() const;
  bool ReadsPrimaryRow() const;
  bool WritesStaticRow() const;
  bool WritesPrimaryRow() const;

  void set_writes_static_row(const bool value) { writes_static_row_ = value; }
  void set_writes_primary_row(const bool value) { writes_primary_row_ = value; }

 protected:
  Type type() const override { return QL_WRITE; }

 private:
  friend class YBTable;
  static std::unique_ptr<YBqlWriteOp> NewInsert(const std::shared_ptr<YBTable>& table);
  static std::unique_ptr<YBqlWriteOp> NewUpdate(const std::shared_ptr<YBTable>& table);
  static std::unique_ptr<YBqlWriteOp> NewDelete(const std::shared_ptr<YBTable>& table);
  std::unique_ptr<QLWriteRequestPB> ql_write_request_;

  // Does this operation write to the static or primary row?
  bool writes_static_row_ = false;
  bool writes_primary_row_ = false;
};

class YBqlReadOp : public YBqlOp {
 public:
  ~YBqlReadOp();

  static std::unique_ptr<YBqlReadOp> NewSelect(const std::shared_ptr<YBTable>& table);

  // Note: to avoid memory copy, this QLReadRequestPB is moved into tserver ReadRequestPB
  // when the request is sent to tserver. It is restored after response is received from tserver
  // (see ReadRpc's constructor).
  const QLReadRequestPB& request() const { return *ql_read_request_; }

  QLReadRequestPB* mutable_request() { return ql_read_request_.get(); }

  std::string ToString() const override;

  bool read_only() const override { return true; };

  bool returns_sidecar() override { return true; }

  void SetHashCode(uint16_t hash_code) override;

  // Returns the partition key of the read request if it exists.
  // Also sets the hash_code and max_hash_code in the request.
  CHECKED_STATUS GetPartitionKey(std::string* partition_key) const override;

  const YBConsistencyLevel yb_consistency_level() {
    return yb_consistency_level_;
  }

  void set_yb_consistency_level(const YBConsistencyLevel yb_consistency_level) {
    yb_consistency_level_ = yb_consistency_level;
  }

  std::vector<ColumnSchema> MakeColumnSchemasFromRequest() const;
  Result<QLRowBlock> MakeRowBlock() const;

  const ReadHybridTime& read_time() const { return read_time_; }
  void SetReadTime(const ReadHybridTime& value) { read_time_ = value; }

 protected:
  Type type() const override { return QL_READ; }
  OpGroup group() override;

 private:
  friend class YBTable;
  explicit YBqlReadOp(const std::shared_ptr<YBTable>& table);
  std::unique_ptr<QLReadRequestPB> ql_read_request_;
  YBConsistencyLevel yb_consistency_level_;
  ReadHybridTime read_time_;
};

std::vector<ColumnSchema> MakeColumnSchemasFromColDesc(
  const google::protobuf::RepeatedPtrField<QLRSColDescPB>& rscol_descs);

//--------------------------------------------------------------------------------------------------
// YB Postgresql Operators.
//--------------------------------------------------------------------------------------------------

class YBPgsqlOp : public YBOperation {
 public:
  explicit YBPgsqlOp(const std::shared_ptr<YBTable>& table);
  ~YBPgsqlOp();

  const PgsqlResponsePB& response() const { return *response_; }

  PgsqlResponsePB* mutable_response() { return response_.get(); }

  std::string&& rows_data() { return std::move(rows_data_); }

  std::string* mutable_rows_data() { return &rows_data_; }

  bool succeeded() const override {
    return response().status() == PgsqlResponsePB::PGSQL_STATUS_OK;
  }

  bool applied() override {
    return succeeded() && !response_->skipped();
  }

  bool is_active() const {
    return is_active_;
  }

  void set_active(bool val) {
    is_active_ = val;
  }

 protected:
  std::unique_ptr<PgsqlResponsePB> response_;
  std::string rows_data_;

  // This flag is only meaningful in PgGate (proxy / client).
  // To support parallel processing by partitions or hash-codes, client will create many operators,
  // one per tablet with a specific partition range. When none of users' input arguments have the
  // partition value within the ranges that are associated with certain operators, those operators
  // would be set in-active, and their op->request() would not be sent to tablet server.
  bool is_active_ = true;
};

class YBPgsqlWriteOp : public YBPgsqlOp {
 public:
  explicit YBPgsqlWriteOp(const std::shared_ptr<YBTable>& table);
  ~YBPgsqlWriteOp();

  // Copying all member data except for response_ and rows_data_.
  // - Input (hash_code, bind expresion) of the copy will be modified during execution.
  // - Output (response_, rows_data_) arguments will be read from DocDB.
  std::unique_ptr<YBPgsqlWriteOp> DeepCopy();

  // Note: to avoid memory copy, this PgsqlWriteRequestPB is moved into tserver WriteRequestPB
  // when the request is sent to tserver. It is restored after response is received from tserver
  // (see WriteRpc's constructor).
  const PgsqlWriteRequestPB& request() const { return *write_request_; }

  PgsqlWriteRequestPB* mutable_request() { return write_request_.get(); }

  std::string ToString() const override;

  bool read_only() const override { return false; };

  // TODO check for e.g. returning clause.
  bool returns_sidecar() override { return true; }

  void SetHashCode(uint16_t hash_code) override;

  CHECKED_STATUS GetPartitionKey(std::string* partition_key) const override;

  bool IsTransactional() const override;

  void set_is_single_row_txn(bool is_single_row_txn) {
    is_single_row_txn_ = is_single_row_txn;
  }

  const HybridTime& write_time() const { return write_time_; }
  void SetWriteTime(const HybridTime& value) { write_time_ = value; }

 protected:
  virtual Type type() const override { return PGSQL_WRITE; }

 private:
  friend class YBTable;
  static std::unique_ptr<YBPgsqlWriteOp> NewInsert(const std::shared_ptr<YBTable>& table);
  static std::unique_ptr<YBPgsqlWriteOp> NewUpdate(const std::shared_ptr<YBTable>& table);
  static std::unique_ptr<YBPgsqlWriteOp> NewDelete(const std::shared_ptr<YBTable>& table);
  static std::unique_ptr<YBPgsqlWriteOp> NewTruncateColocated(
      const std::shared_ptr<YBTable>& table);

  std::unique_ptr<PgsqlWriteRequestPB> write_request_;
  // Whether this operation should be run as a single row txn.
  // Else could be distributed transaction (or non-transactional) depending on target table type.
  bool is_single_row_txn_ = false;
  HybridTime write_time_;
};

class YBPgsqlReadOp : public YBPgsqlOp {
 public:
  static std::unique_ptr<YBPgsqlReadOp> NewSelect(const std::shared_ptr<YBTable>& table);

  // Create a deep copy of this operation, copying all fields and request PB content.
  // Does NOT, however, copy response and rows data.
  std::unique_ptr<YBPgsqlReadOp> DeepCopy();

  // Note: to avoid memory copy, this PgsqlReadRequestPB is moved into tserver ReadRequestPB
  // when the request is sent to tserver. It is restored after response is received from tserver
  // (see ReadRpc's constructor).
  const PgsqlReadRequestPB& request() const { return *read_request_; }

  PgsqlReadRequestPB* mutable_request() { return read_request_.get(); }

  std::string ToString() const override;

  bool read_only() const override { return true; };

  bool returns_sidecar() override { return true; }

  void SetHashCode(uint16_t hash_code) override;

  // Returns the partition key of the read request if it exists.
  // Also sets the hash_code and max_hash_code in the request.
  CHECKED_STATUS GetPartitionKey(std::string* partition_key) const override;

  const YBConsistencyLevel yb_consistency_level() {
    return yb_consistency_level_;
  }

  void set_yb_consistency_level(const YBConsistencyLevel yb_consistency_level) {
    yb_consistency_level_ = yb_consistency_level;
  }

  std::vector<ColumnSchema> MakeColumnSchemasFromRequest() const;
  Result<QLRowBlock> MakeRowBlock() const;

  const ReadHybridTime& read_time() const { return read_time_; }
  void SetReadTime(const ReadHybridTime& value) { read_time_ = value; }
  void SetIsForBackfill(const bool value) { read_request_->set_is_for_backfill(value); }

  static std::vector<ColumnSchema> MakeColumnSchemasFromColDesc(
      const google::protobuf::RepeatedPtrField<PgsqlRSColDescPB>& rscol_descs);

  bool should_add_intents(IsolationLevel isolation_level) override;

 protected:
  virtual Type type() const override { return PGSQL_READ; }
  OpGroup group() override;

 private:
  friend class YBTable;

  explicit YBPgsqlReadOp(const std::shared_ptr<YBTable>& table);
  CHECKED_STATUS GetHashPartitionKey(std::string* partition_key) const;
  CHECKED_STATUS GetRangePartitionKey(std::string* partition_key) const;

  std::unique_ptr<PgsqlReadRequestPB> read_request_;
  YBConsistencyLevel yb_consistency_level_ = YBConsistencyLevel::STRONG;
  ReadHybridTime read_time_;
};

// This class is not thread-safe, though different YBNoOp objects on
// different threads may share a single YBTable object.
class YBNoOp {
 public:
  // Initialize the NoOp request object. The given 'table' object must remain valid
  // for the lifetime of this object.
  explicit YBNoOp(const std::shared_ptr<YBTable>& table);

  // Executes a no-op request against the tablet server on which the row specified
  // by "key" lives.
  CHECKED_STATUS Execute(const YBPartialRow& key);
 private:
  const std::shared_ptr<YBTable> table_;

  DISALLOW_COPY_AND_ASSIGN(YBNoOp);
};

CHECKED_STATUS ReviewResponsePagingState(YBPgsqlReadOp* op);

}  // namespace client
}  // namespace yb

#endif  // YB_CLIENT_YB_OP_H_
