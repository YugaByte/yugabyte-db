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

#ifndef YB_YQL_PGWRAPPER_LIBPQ_UTILS_H
#define YB_YQL_PGWRAPPER_LIBPQ_UTILS_H

#include <libpq-fe.h>

#include <memory>

#include "yb/common/common.pb.h"

#include "yb/util/net/net_util.h"
#include "yb/util/result.h"

namespace yb {
namespace pgwrapper {

struct PGConnClose {
  void operator()(PGconn* conn) const;
};

struct PGResultClear {
  void operator()(PGresult* result) const;
};

typedef std::unique_ptr<PGresult, PGResultClear> PGResultPtr;

Result<int32_t> GetInt32(PGresult* result, int row, int column);

Result<int64_t> GetInt64(PGresult* result, int row, int column);

Result<double> GetDouble(PGresult* result, int row, int column);

Result<std::string> GetString(PGresult* result, int row, int column);

inline Result<int32_t> GetValueImpl(PGresult* result, int row, int column, int32_t*) {
  return GetInt32(result, row, column);
}

inline Result<int64_t> GetValueImpl(PGresult* result, int row, int column, int64_t*) {
  return GetInt64(result, row, column);
}

inline Result<std::string> GetValueImpl(PGresult* result, int row, int column, std::string*) {
  return GetString(result, row, column);
}

template<class T>
Result<T> GetValue(PGresult* result, int row, int column) {
  // static_cast<T*>(nullptr) is a trick to use function overload from template.
  return GetValueImpl(result, row, column, static_cast<T*>(nullptr));
}

Result<std::string> AsString(PGresult* result, int row, int column);
void LogResult(PGresult* result);

class PGConn {
 public:
  ~PGConn();

  PGConn(PGConn&& rhs);
  PGConn& operator=(PGConn&& rhs);

  static Result<PGConn> Connect(const HostPort& host_port, const std::string& db_name = "");

  CHECKED_STATUS Execute(const std::string& command);

  template <class... Args>
  CHECKED_STATUS ExecuteFormat(const std::string& format, Args&&... args) {
    return Execute(Format(format, std::forward<Args>(args)...));
  }

  Result<PGResultPtr> Fetch(const std::string& command);

  template <class... Args>
  Result<PGResultPtr> FetchFormat(const std::string& format, Args&&... args) {
    return Fetch(Format(format, std::forward<Args>(args)...));
  }

  // Fetches data matrix of specified size. I.e. exact number of rows and columns are expected.
  Result<PGResultPtr> FetchMatrix(const std::string& command, int rows, int columns);

  template <class T>
  Result<T> FetchValue(const std::string& command) {
    auto res = VERIFY_RESULT(FetchMatrix(command, 1, 1));
    return GetValue<T>(res.get(), 0, 0);
  }

  CHECKED_STATUS StartTransaction(IsolationLevel isolation_level);
  CHECKED_STATUS CommitTransaction();
  CHECKED_STATUS RollbackTransaction();

  // Would this query use an index [only] scan?
  Result<bool> HasIndexScan(const std::string& query);

  CHECKED_STATUS CopyBegin(const std::string& command);
  Result<PGResultPtr> CopyEnd();

  void CopyStartRow(int16_t columns);

  void CopyPutInt16(int16_t value);
  void CopyPutInt32(int32_t value);
  void CopyPutInt64(int64_t value);
  void CopyPut(const char* value, size_t len);

  void CopyPutString(const std::string& value) {
    CopyPut(value.c_str(), value.length());
  }

  PGconn* get() {
    return impl_.get();
  }

 private:
  typedef std::unique_ptr<PGconn, PGConnClose> PGConnPtr;
  struct CopyData;

  explicit PGConn(PGConnPtr ptr);

  bool CopyEnsureBuffer(size_t len);
  bool CopyFlushBuffer();

  PGConnPtr impl_;
  std::unique_ptr<CopyData> copy_data_;
};

} // namespace pgwrapper
} // namespace yb

#endif // YB_YQL_PGWRAPPER_LIBPQ_UTILS_H
