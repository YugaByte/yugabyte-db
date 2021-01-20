//
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
//

#include "yb/client/ql-dml-test-base.h"

#include "yb/client/client.h"
#include "yb/client/error.h"
#include "yb/client/session.h"
#include "yb/client/table.h"

#include "yb/common/ql_name.h"
#include "yb/common/ql_value.h"

#include "yb/util/bfql/gen_opcodes.h"

#include "yb/yql/cql/ql/util/errcodes.h"
#include "yb/yql/cql/ql/util/statement_result.h"

DECLARE_bool(enable_ysql);

using namespace std::literals;

namespace yb {
namespace client {

const client::YBTableName kTableName(YQL_DATABASE_CQL, "my_keyspace", "ql_client_test_table");

template<class MiniClusterType>
const std::string KeyValueTableTest<MiniClusterType>::kKeyColumn = kv_table_test::kKeyColumn;

template<class MiniClusterType>
const std::string KeyValueTableTest<MiniClusterType>::kValueColumn = kv_table_test::kValueColumn;

template <>
QLDmlTestBase<MiniCluster>::QLDmlTestBase() : mini_cluster_opt_(1, 3) {}

template <>
void QLDmlTestBase<MiniCluster>::SetFlags() {
  SetAtomicFlag(false, &FLAGS_enable_ysql);
}

template <>
void QLDmlTestBase<MiniCluster>::StartCluster() {
  cluster_.reset(new MiniCluster(env_.get(), mini_cluster_opt_));
  ASSERT_OK(cluster_->Start());
}

template<class MiniClusterType>
void QLDmlTestBase<MiniClusterType>::SetUp() {
  SetFlags();
  HybridTime::TEST_SetPrettyToString(true);

  YBMiniClusterTestBase<MiniClusterType>::SetUp();

  // Start minicluster and wait for tablet servers to connect to master.
  StartCluster();

  ASSERT_OK(this->CreateClient());

  // Create test namespace.
  ASSERT_OK(client_->CreateNamespaceIfNotExists(kTableName.namespace_name(),
                                                kTableName.namespace_type()));
}

template <class MiniClusterType>
void QLDmlTestBase<MiniClusterType>::DoTearDown() {
  // If we enable this, it will break FLAGS_mini_cluster_reuse_data
  //
  // This DeleteTable clean up seems to cause a crash because the delete may not succeed
  // immediately and is retried after the master is restarted (see ENG-663). So disable it for
  // now.
  //
  // if (table_) {
  //   ASSERT_OK(client_->DeleteTable(kTableName));
  // }
  MiniClusterTestWithClient<MiniClusterType>::DoTearDown();
}

template class QLDmlTestBase<MiniCluster>;

namespace kv_table_test {

namespace {

QLWriteRequestPB::QLStmtType GetQlStatementType(const WriteOpType op_type) {
  switch (op_type) {
    case WriteOpType::INSERT:
      return QLWriteRequestPB::QL_STMT_INSERT;
    case WriteOpType::UPDATE:
      return QLWriteRequestPB::QL_STMT_UPDATE;
    case WriteOpType::DELETE:
      return QLWriteRequestPB::QL_STMT_DELETE;
  }
  FATAL_INVALID_ENUM_VALUE(WriteOpType, op_type);
}

} // namespace

Result<YBqlWriteOpPtr> Increment(
    TableHandle* table, const YBSessionPtr& session, int32_t key, int32_t delta) {
  auto op = table->NewWriteOp(QLWriteRequestPB::QL_STMT_UPDATE);
  auto value_column_id = table->ColumnId(kValueColumn);

  auto* const req = op->mutable_request();
  QLAddInt32HashValue(req, key);
  req->mutable_column_refs()->add_ids(value_column_id);
  auto* column_value = req->add_column_values();
  column_value->set_column_id(value_column_id);
  auto* bfcall = column_value->mutable_expr()->mutable_bfcall();
  bfcall->set_opcode(to_underlying(bfql::BFOpcode::OPCODE_ConvertI64ToI32_18));
  bfcall = bfcall->add_operands()->mutable_bfcall();

  bfcall->set_opcode(to_underlying(bfql::BFOpcode::OPCODE_AddI64I64_80));
  auto column_op = bfcall->add_operands()->mutable_bfcall();
  column_op->set_opcode(to_underlying(bfql::BFOpcode::OPCODE_ConvertI32ToI64_13));
  column_op->add_operands()->set_column_id(value_column_id);
  bfcall->add_operands()->mutable_value()->set_int64_value(delta);

  RETURN_NOT_OK(session->Apply(op));

  return op;
}

void CreateTable(
    Transactional transactional, int num_tablets, YBClient* client, TableHandle* table) {
  ASSERT_OK(client->CreateNamespaceIfNotExists(kTableName.namespace_name(),
                                               kTableName.namespace_type()));

  YBSchemaBuilder builder;
  builder.AddColumn(kKeyColumn)->Type(INT32)->HashPrimaryKey()->NotNull();
  builder.AddColumn(kValueColumn)->Type(INT32);
  if (transactional) {
    TableProperties table_properties;
    table_properties.SetTransactional(true);
    builder.SetTableProperties(table_properties);
  }

  ASSERT_OK(table->Create(kTableName, num_tablets, client, &builder));
}

void CreateIndex(
    Transactional transactional,
    int indexed_column_index,
    bool use_mangled_names,
    const TableHandle& table,
    YBClient* client,
    TableHandle* index) {
  const YBSchema& schema = table.schema();
  DCHECK_LT(indexed_column_index, schema.num_columns());

  // When creating an index, we construct IndexInfo and associated it with the data-table.
  IndexInfoPB index_info;
  index_info.set_indexed_table_id(table->id());
  index_info.set_is_local(false);
  index_info.set_is_unique(false);
  index_info.set_use_mangled_column_name(use_mangled_names);

  // List key columns of data-table being indexed.
  index_info.set_hash_column_count(1);
  index_info.add_indexed_hash_column_ids(schema.ColumnId(0));

  auto* column = index_info.add_columns();
  const string name = schema.Column(indexed_column_index).name();
  column->set_column_name(use_mangled_names ? YcqlName::MangleColumnName(name) : name);
  column->set_indexed_column_id(schema.ColumnId(indexed_column_index));

  // Setup Index table schema.
  YBSchemaBuilder builder;
  builder.AddColumn(use_mangled_names ? YcqlName::MangleColumnName(name) : name)
      ->Type(schema.Column(indexed_column_index).type())
      ->NotNull()
      ->HashPrimaryKey();

  size_t num_range_keys = 0;
  for (size_t i = 0; i < schema.num_hash_key_columns(); ++i) {
    if (i != indexed_column_index) {
      const string name = schema.Column(i).name();
      builder.AddColumn(use_mangled_names ? YcqlName::MangleColumnName(name) : name)
          ->Type(schema.Column(i).type())
          ->NotNull()
          ->PrimaryKey();

      column = index_info.add_columns();
      column->set_column_name(use_mangled_names ? YcqlName::MangleColumnName(name) : name);
      column->set_indexed_column_id(schema.ColumnId(i));
      ++num_range_keys;
    }
  }

  index_info.set_range_column_count(num_range_keys);
  TableProperties table_properties;
  table_properties.SetUseMangledColumnName(use_mangled_names);

  if (transactional) {
    table_properties.SetTransactional(true);
  }

  builder.SetTableProperties(table_properties);

  const YBTableName index_name(YQL_DATABASE_CQL, table.name().namespace_name(),
      table.name().table_name() + '_' + schema.Column(indexed_column_index).name() + "_idx");

  ASSERT_OK(index->Create(index_name, schema.table_properties().num_tablets(),
      client, &builder, &index_info));
}

Result<YBqlWriteOpPtr> WriteRow(
    TableHandle* table, const YBSessionPtr& session, int32_t key, int32_t value,
    const WriteOpType op_type, Flush flush) {
  VLOG(4) << "Calling WriteRow key=" << key << " value=" << value << " op_type="
          << yb::ToString(op_type);
  const QLWriteRequestPB::QLStmtType stmt_type = GetQlStatementType(op_type);
  const auto op = table->NewWriteOp(stmt_type);
  auto* const req = op->mutable_request();
  QLAddInt32HashValue(req, key);
  if (op_type != WriteOpType::DELETE) {
    table->AddInt32ColumnValue(req, kValueColumn, value);
  }
  RETURN_NOT_OK(session->Apply(op));
  if (flush) {
    RETURN_NOT_OK(session->Flush());
    RETURN_NOT_OK(CheckOp(op.get()));
  }
  return op;
}

Result<YBqlWriteOpPtr> DeleteRow(
    TableHandle* table, const YBSessionPtr& session, int32_t key) {
  return kv_table_test::WriteRow(table, session, key, 0 /* value */, WriteOpType::DELETE);
}

Result<YBqlWriteOpPtr> UpdateRow(
    TableHandle* table, const YBSessionPtr& session, int32_t key, int32_t value) {
  return kv_table_test::WriteRow(table, session, key, value, WriteOpType::UPDATE);
}

Result<int32_t> SelectRow(
    TableHandle* table, const YBSessionPtr& session, int32_t key, const std::string& column) {
  const YBqlReadOpPtr op = table->NewReadOp();
  auto* const req = op->mutable_request();
  QLAddInt32HashValue(req, key);
  table->AddColumns({column}, req);
  auto status = session->ApplyAndFlush(op);
  if (status.IsIOError()) {
    for (const auto& error : session->GetAndClearPendingErrors()) {
      LOG(WARNING) << "Error: " << error->status() << ", op: " << error->failed_op().ToString();
    }
  }
  RETURN_NOT_OK(status);
  RETURN_NOT_OK(CheckOp(op.get()));
  auto rowblock = yb::ql::RowsResult(op.get()).GetRowBlock();
  if (rowblock->row_count() == 0) {
    return STATUS_FORMAT(NotFound, "Row not found for key $0", key);
  }
  return rowblock->row(0).column(0).int32_value();
}

Result<std::map<int32_t, int32_t>> SelectAllRows(
    TableHandle* table, const YBSessionPtr& session) {
  std::vector<YBqlReadOpPtr> ops;
  auto partitions = table->table()->GetPartitionsCopy();
  partitions.push_back(std::string()); // Upper bound for last partition.

  uint16_t prev_code = 0;
  for (const auto& partition : partitions) {
    const YBqlReadOpPtr op = table->NewReadOp();
    auto* const req = op->mutable_request();
    table->AddColumns(table->AllColumnNames(), req);
    if (prev_code) {
      req->set_hash_code(prev_code);
    }
    // Partition could be empty, or contain 2 bytes of partition start.
    if (partition.size() == 2) {
      uint16_t current_code = BigEndian::Load16(partition.c_str());
      req->set_max_hash_code(current_code - 1);
      prev_code = current_code;
    } else if (!prev_code) {
      // Partitions contain starts of partition, so we always skip first iteration, because don't
      // know end of first partition at this point.
      continue;
    }
    ops.push_back(op);
    RETURN_NOT_OK(session->Apply(op));
  }

  RETURN_NOT_OK(session->Flush());

  std::map<int32_t, int32_t> result;
  for (const auto& op : ops) {
    RETURN_NOT_OK(CheckOp(op.get()));
    auto rowblock = yb::ql::RowsResult(op.get()).GetRowBlock();
    for (const auto& row : rowblock->rows()) {
      result.emplace(row.column(0).int32_value(), row.column(1).int32_value());
    }
  }

  return result;
}

} // namespace kv_table_test

template <class MiniClusterType>
void KeyValueTableTest<MiniClusterType>::CreateTable(Transactional transactional) {
  kv_table_test::CreateTable(transactional, NumTablets(), client_.get(), &table_);
}

template <class MiniClusterType>
void KeyValueTableTest<MiniClusterType>::CreateIndex(
    Transactional transactional, int indexed_column_index, bool use_mangled_names) {
  kv_table_test::CreateIndex(
      transactional, indexed_column_index, use_mangled_names, table_, client_.get(), &index_);
}

template <class MiniClusterType>
int KeyValueTableTest<MiniClusterType>::NumTablets() {
  return num_tablets_;
}

template <class MiniClusterType>
YBSessionPtr KeyValueTableTest<MiniClusterType>::CreateSession(
    const YBTransactionPtr& transaction, const server::ClockPtr& clock) {
  auto session = std::make_shared<YBSession>(client_.get(), clock);
  if (transaction) {
    session->SetTransaction(transaction);
  }
  session->SetTimeout(RegularBuildVsSanitizers(15s, 60s));
  return session;
}

template class KeyValueTableTest<MiniCluster>;

Status CheckOp(YBqlOp* op) {
  if (!op->succeeded()) {
    return STATUS(QLError,
                  op->response().error_message(),
                  Slice(),
                  ql::QLError(ql::QLStatusToErrorCode(op->response().status())));
  }

  return Status::OK();
}

} // namespace client
} // namespace yb
