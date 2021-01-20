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

#ifndef YB_INTEGRATION_TESTS_CQL_TEST_UTIL_H
#define YB_INTEGRATION_TESTS_CQL_TEST_UTIL_H

#include <cassandra.h>

#include <string>

#include "yb/util/monotime.h"
#include "yb/util/result.h"

namespace yb {

class CassandraJson;
class CassandraStatement;

// Cassandra CPP driver has his own functions to release objects, so we should use them for it.
template <class T, void (*Func)(T*)>
class FuncDeleter {
 public:
  void operator()(T* t) const {
    if (t) {
      Func(t);
    }
  }
};

template <class Out>
struct GetCassandraValue {
  static CassError Apply(const CassValue* value, Out* out);
};

class CassandraValue {
 public:
  explicit CassandraValue(const CassValue* value) : value_(value) {}

  template <class Out>
  void Get(Out* out) const {
    CHECK_EQ(CASS_OK, GetCassandraValue<Out>::Apply(value_, out));
  }

  template <class Out>
  Out As() const {
    Out result;
    Get(&result);
    return result;
  }

  std::string ToString() const;

 private:
  const CassValue* value_;
};

typedef std::unique_ptr<
    CassIterator, FuncDeleter<CassIterator, &cass_iterator_free>> CassIteratorPtr;

class CassandraRowIterator {
 public:
  explicit CassandraRowIterator(CassIterator* iterator) : cass_iterator_(iterator) {}

  bool Next();

  template <class Out>
  void Get(Out* out) const {
    Value().Get(out);
  }

  CassandraValue Value() const;

 private:
  CassIteratorPtr cass_iterator_;
};

class CassandraRow {
 public:
  explicit CassandraRow(const CassRow* row) : cass_row_(row) {}

  template <class Out>
  void Get(size_t index, Out* out) const {
    return Value(index).Get(out);
  }

  CassandraValue Value(size_t index) const;

  CassandraRowIterator CreateIterator() const;

  void TakeIterator(CassIteratorPtr iterator);

 private:
  const CassRow* cass_row_; // owned by iterator
  CassIteratorPtr cass_iterator_;
};

class CassandraIterator {
 public:
  explicit CassandraIterator(CassIterator* iterator) : cass_iterator_(iterator) {}

  bool Next();

  CassandraRow Row();

  void MoveToRow(CassandraRow* row);

 private:
  CassIteratorPtr cass_iterator_;
};

typedef std::unique_ptr<
    const CassResult, FuncDeleter<const CassResult, &cass_result_free>> CassResultPtr;

class CassandraResult {
 public:
  explicit CassandraResult(const CassResult* result) : cass_result_(result) {}

  CassandraIterator CreateIterator() const;

 private:
  CassResultPtr cass_result_;
};

typedef std::unique_ptr<
    const CassPrepared, FuncDeleter<const CassPrepared, &cass_prepared_free>> CassPreparedPtr;

class CassandraPrepared {
 public:
  CassandraPrepared() = default;
  explicit CassandraPrepared(const CassPrepared* prepared) : prepared_(prepared) {}

  CassandraStatement Bind();

 private:
  CassPreparedPtr prepared_;
};

typedef std::unique_ptr<
    CassFuture, FuncDeleter<CassFuture, &cass_future_free>> CassFuturePtr;

class CassandraFuture {
 public:
  explicit CassandraFuture(CassFuture* future) : future_(future) {}

  bool Ready() const;

  CHECKED_STATUS Wait();

  CHECKED_STATUS WaitFor(MonoDelta duration);

  CassandraResult Result();

  CassandraPrepared Prepared();

 private:
  CHECKED_STATUS CheckErrorCode();

  CassFuturePtr future_;
};

typedef std::unique_ptr<
    CassStatement, FuncDeleter<CassStatement, &cass_statement_free>> CassStatementPtr;

class CassandraStatement {
 public:
  explicit CassandraStatement(CassStatement* statement)
      : cass_statement_(statement) {}

  explicit CassandraStatement(const std::string& query, size_t parameter_count = 0)
      : cass_statement_(cass_statement_new(query.c_str(), parameter_count)) {}

  void Bind(size_t index, const std::string& v);
  void Bind(size_t index, const cass_bool_t& v);
  void Bind(size_t index, const cass_float_t& v);
  void Bind(size_t index, const cass_double_t& v);
  void Bind(size_t index, const cass_int32_t& v);
  void Bind(size_t index, const cass_int64_t& v);
  void Bind(size_t index, const CassandraJson& v);

  CassStatement* get() const;

 private:
  friend class CassandraBatch;
  friend class CassandraSession;

  CassStatementPtr cass_statement_;
};

typedef std::unique_ptr<CassBatch, FuncDeleter<CassBatch, &cass_batch_free>> CassBatchPtr;

class CassandraBatch {
 public:
  explicit CassandraBatch(CassBatchType type) : cass_batch_(cass_batch_new(type)) {}

  void Add(CassandraStatement* statement);

 private:
  friend class CassandraSession;

  CassBatchPtr cass_batch_;
};

struct DeleteSession {
  void operator()(CassSession* session) const;
};

typedef std::unique_ptr<CassSession, DeleteSession> CassSessionPtr;

class CassandraSession {
 public:
  CassandraSession() = default;

  CHECKED_STATUS Connect(CassCluster* cluster);

  static Result<CassandraSession> Create(CassCluster* cluster);

  CHECKED_STATUS Execute(const CassandraStatement& statement);

  Result<CassandraResult> ExecuteWithResult(const CassandraStatement& statement);

  CassandraFuture ExecuteGetFuture(const CassandraStatement& statement);

  CassandraFuture ExecuteGetFuture(const std::string& query);

  CHECKED_STATUS ExecuteQuery(const std::string& query);

  Result<CassandraResult> ExecuteWithResult(const std::string& query);

  template <class Action>
  CHECKED_STATUS ExecuteAndProcessOneRow(
      const CassandraStatement& statement, const Action& action) {
    auto result = VERIFY_RESULT(ExecuteWithResult(statement));
    auto iterator = result.CreateIterator();
    if (!iterator.Next()) {
      return STATUS(IllegalState, "Row does not exists");
    }
    auto row = iterator.Row();
    action(row);
    if (iterator.Next()) {
      return STATUS(IllegalState, "Multiple rows returned");
    }
    return Status::OK();
  }

  template <class Action>
  CHECKED_STATUS ExecuteAndProcessOneRow(const std::string& query, const Action& action) {
    return ExecuteAndProcessOneRow(CassandraStatement(query), action);
  }

  CHECKED_STATUS ExecuteBatch(const CassandraBatch& batch);

  CassandraFuture SubmitBatch(const CassandraBatch& batch);

  Result<CassandraPrepared> Prepare(
      const std::string& prepare_query, MonoDelta timeout = MonoDelta::kZero);

  void Reset();

 private:
  CassSessionPtr cass_session_;
};

class CppCassandraDriver {
 public:
  CppCassandraDriver(
      const std::vector<std::string>& hosts, uint16_t port, bool use_partition_aware_routing);

  ~CppCassandraDriver();

  Result<CassandraSession> CreateSession();

 private:
  CassCluster* cass_cluster_ = nullptr;
};

class CassandraJson {
 public:
  CassandraJson() = default;
  explicit CassandraJson(const std::string& s) : value_(s) {}
  explicit CassandraJson(std::string&& s) : value_(std::move(s)) {}
  explicit CassandraJson(const char* s) : value_(s) {}

  const std::string& value() const {
    return value_;
  }

 private:
  std::string value_;
};

inline std::ostream& operator<<(std::ostream& out, const CassandraJson& value) {
  return out << value.value();
}

inline bool operator==(const CassandraJson& lhs, const CassandraJson& rhs) {
  return lhs.value() == rhs.value();
}

extern const MonoDelta kCassandraTimeOut;

Result<CassandraSession> EstablishSession(CppCassandraDriver* driver);

} // namespace yb

#endif // YB_INTEGRATION_TESTS_CQL_TEST_UTIL_H
