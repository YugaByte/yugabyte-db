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

#ifndef YB_CLIENT_QL_DML_TEST_BASE_H
#define YB_CLIENT_QL_DML_TEST_BASE_H

#include <algorithm>
#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include "yb/client/client.h"
#include "yb/client/yb_op.h"
#include "yb/client/callbacks.h"
#include "yb/client/table_handle.h"
#include "yb/common/ql_protocol.pb.h"
#include "yb/common/ql_rowblock.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/yb_mini_cluster_test_base.h"
#include "yb/master/mini_master.h"
#include "yb/tablet/tablet_fwd.h"
#include "yb/util/async_util.h"
#include "yb/util/test_util.h"

namespace yb {
namespace client {

extern const client::YBTableName kTableName;

class QLDmlTestBase : public MiniClusterTestWithClient<MiniCluster> {
 public:
  void SetUp() override;
  void DoTearDown() override;

  virtual ~QLDmlTestBase() {}

 protected:
  MiniClusterOptions mini_cluster_opt_ = MiniClusterOptions(1, 3);
};

YB_STRONGLY_TYPED_BOOL(Transactional);
YB_DEFINE_ENUM(WriteOpType, (INSERT)(UPDATE)(DELETE));
YB_STRONGLY_TYPED_BOOL(Flush);

class KeyValueTableTest : public QLDmlTestBase {
 public:
  static void CreateTable(Transactional transactional, int num_tablets, YBClient* client,
                          TableHandle* table);

  static void CreateIndex(Transactional transactional, int indexed_column_index,
                          bool use_mangled_names, const TableHandle& table,
                          YBClient* client, TableHandle* index);

  // Insert/update a full, single row, equivalent to the statement below. Return a YB write op that
  // has been applied.
  // op_type == WriteOpType::INSERT: insert into t values (key, value);
  // op_type == WriteOpType::UPDATE: update t set v=value where k=key;
  // op_type == WriteOpType::DELETE: delete from t where k=key; (parameter "value" is unused).
  static Result<YBqlWriteOpPtr> WriteRow(
      TableHandle* table, const YBSessionPtr& session, int32_t key, int32_t value,
      const WriteOpType op_type = WriteOpType::INSERT,
      Flush flush = Flush::kTrue);

  static Result<YBqlWriteOpPtr> DeleteRow(
      TableHandle* table, const YBSessionPtr& session, int32_t key);

  static Result<YBqlWriteOpPtr> UpdateRow(
      TableHandle* table, const YBSessionPtr& session, int32_t key, int32_t value);

  // Select the specified columns of a row using a primary key, equivalent to the select statement
  // below. Return a YB read op that has been applied.
  //   select <columns...> from t where h1 = <h1> and h2 = <h2> and r1 = <r1> and r2 = <r2>;
  static Result<int32_t> SelectRow(
      TableHandle* table, const YBSessionPtr& session, int32_t key,
      const std::string& column = kValueColumn);

  // Selects all rows from test table, returning map key => value.
  static Result<std::map<int32_t, int32_t>> SelectAllRows(
      TableHandle* table, const YBSessionPtr& session);

  static Result<YBqlWriteOpPtr> Increment(
      TableHandle* table, const YBSessionPtr& session, int32_t key, int32_t delta = 1);

 protected:
  void CreateTable(Transactional transactional);

  void CreateIndex(Transactional transactional,
                   int indexed_column_index = 1,
                   bool use_mangled_names = true);

  Result<YBqlWriteOpPtr> WriteRow(
      const YBSessionPtr& session, int32_t key, int32_t value,
      const WriteOpType op_type = WriteOpType::INSERT,
      Flush flush = Flush::kTrue) {
    return WriteRow(&table_, session, key, value, op_type, flush);
  }

  Result<YBqlWriteOpPtr> DeleteRow(const YBSessionPtr& session, int32_t key) {
    return DeleteRow(&table_, session, key);
  }

  Result<YBqlWriteOpPtr> UpdateRow(const YBSessionPtr& session, int32_t key, int32_t value) {
    return UpdateRow(&table_, session, key, value);
  }

  // Select the specified columns of a row using a primary key, equivalent to the select statement
  // below. Return a YB read op that has been applied.
  //   select <columns...> from t where h1 = <h1> and h2 = <h2> and r1 = <r1> and r2 = <r2>;
  Result<int32_t> SelectRow(const YBSessionPtr& session, int32_t key,
                            const std::string& column = kValueColumn) {
    return SelectRow(&table_, session, key, column);
  }

  YBSessionPtr CreateSession(const YBTransactionPtr& transaction = nullptr,
                             const server::ClockPtr& clock = nullptr);

  // Selects all rows from test table, returning map key => value.
  Result<std::map<int32_t, int32_t>> SelectAllRows(const YBSessionPtr& session) {
    return SelectAllRows(&table_, session);
  }

  virtual int NumTablets();

  // Sets number of tablets to use for test table creation.
  void SetNumTablets(int num_tablets) {
    num_tablets_ = num_tablets;
  }

  static const std::string kKeyColumn;
  static const std::string kValueColumn;
  TableHandle table_;
  TableHandle index_;
  int num_tablets_ = CalcNumTablets(3);
};

CHECKED_STATUS CheckOp(YBqlOp* op);

}  // namespace client
}  // namespace yb

#endif // YB_CLIENT_QL_DML_TEST_BASE_H
