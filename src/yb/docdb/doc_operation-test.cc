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

#include <thread>

#include "yb/rocksdb/statistics.h"
#include "yb/rocksdb/db/column_family.h"
#include "yb/rocksdb/db/internal_stats.h"

#include "yb/common/partial_row.h"
#include "yb/common/ql_protocol_util.h"
#include "yb/common/ql_resultset.h"
#include "yb/common/ql_value.h"
#include "yb/common/transaction-test-util.h"

#include "yb/docdb/cql_operation.h"
#include "yb/docdb/docdb_debug.h"
#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/docdb/docdb_test_base.h"
#include "yb/docdb/docdb_test_util.h"
#include "yb/docdb/doc_rowwise_iterator.h"
#include "yb/docdb/doc_ql_scanspec.h"
#include "yb/docdb/ql_rocksdb_storage.h"
#include "yb/docdb/redis_operation.h"

#include "yb/server/hybrid_clock.h"

#include "yb/util/random_util.h"
#include "yb/util/size_literals.h"
#include "yb/util/tostring.h"

DECLARE_uint64(rocksdb_max_file_size_for_compaction);
DECLARE_int32(rocksdb_level0_slowdown_writes_trigger);
DECLARE_int32(rocksdb_level0_stop_writes_trigger);

using namespace std::literals; // NOLINT

namespace yb {
namespace docdb {

using server::HybridClock;

namespace {

std::vector<ColumnId> CreateColumnIds(size_t count) {
  std::vector<ColumnId> result;
  result.reserve(count);
  for (size_t i = 0; i != count; ++i) {
    result.emplace_back(i);
  }
  return result;
}

constexpr int32_t kFixedHashCode = 0;

} // namespace

class DocOperationTest : public DocDBTestBase {
 public:
  DocOperationTest() {
    SeedRandom();
  }

  Schema CreateSchema() {
    ColumnSchema hash_column_schema("k", INT32, false, true);
    ColumnSchema column1_schema("c1", INT32, false, false);
    ColumnSchema column2_schema("c2", INT32, false, false);
    ColumnSchema column3_schema("c3", INT32, false, false);
    const vector<ColumnSchema> columns({hash_column_schema, column1_schema, column2_schema,
                                           column3_schema});
    Schema schema(columns, CreateColumnIds(columns.size()), 1);
    return schema;
  }

  void AddPrimaryKeyColumn(yb::QLWriteRequestPB* ql_writereq_pb, int32_t value) {
    ql_writereq_pb->add_hashed_column_values()->mutable_value()->set_int32_value(value);
  }

  void AddRangeKeyColumn(int32_t value, yb::QLWriteRequestPB* ql_writereq_pb) {
    ql_writereq_pb->add_range_column_values()->mutable_value()->set_int32_value(value);
  }

  void AddColumnValues(const Schema& schema,
                       const vector<int32_t>& column_values,
                       yb::QLWriteRequestPB* ql_writereq_pb) {
    ASSERT_EQ(schema.num_columns() - schema.num_key_columns(), column_values.size());
    for (int i = 0; i < column_values.size(); i++) {
      auto column = ql_writereq_pb->add_column_values();
      column->set_column_id(schema.num_key_columns() + i);
      column->mutable_expr()->mutable_value()->set_int32_value(column_values[i]);
    }
  }

  void WriteQL(QLWriteRequestPB* ql_writereq_pb, const Schema& schema,
               QLResponsePB* ql_writeresp_pb,
               const HybridTime& hybrid_time = HybridTime::kMax,
               const TransactionOperationContextOpt& txn_op_context =
                   kNonTransactionalOperationContext) {
    QLWriteOperation ql_write_op(std::shared_ptr<const Schema>(&schema, [](const Schema*){}),
                                 IndexMap(), nullptr /* unique_index_key_schema */, txn_op_context);
    ASSERT_OK(ql_write_op.Init(ql_writereq_pb, ql_writeresp_pb));
    auto doc_write_batch = MakeDocWriteBatch();
    HybridTime restart_read_ht;
    ASSERT_OK(ql_write_op.Apply(
        {&doc_write_batch, CoarseTimePoint::max() /* deadline */, ReadHybridTime(),
         &restart_read_ht}));
    ASSERT_OK(WriteToRocksDB(doc_write_batch, hybrid_time));
  }

  void AssertWithTTL(QLWriteRequestPB_QLStmtType stmt_type) {
    if (stmt_type == QLWriteRequestPB::QL_STMT_INSERT) {
      AssertDocDbDebugDumpStrEq(R"#(
SubDocKey(DocKey(0x0000, [1], []), [SystemColumnId(0); HT<max>]) -> null; ttl: 2.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(1); HT{ <max> w: 1 }]) -> 2; ttl: 2.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(2); HT{ <max> w: 2 }]) -> 3; ttl: 2.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(3); HT{ <max> w: 3 }]) -> 4; ttl: 2.000s
      )#");
    } else {
      AssertDocDbDebugDumpStrEq(R"#(
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(1); HT<max>]) -> 2; ttl: 2.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(2); HT{ <max> w: 1 }]) -> 3; ttl: 2.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(3); HT{ <max> w: 2 }]) -> 4; ttl: 2.000s
      )#");
    }
  }

  void AssertWithoutTTL(QLWriteRequestPB_QLStmtType stmt_type) {
    if (stmt_type == QLWriteRequestPB::QL_STMT_INSERT) {
      AssertDocDbDebugDumpStrEq(R"#(
SubDocKey(DocKey(0x0000, [1], []), [SystemColumnId(0); HT<max>]) -> null
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(1); HT{ <max> w: 1 }]) -> 2
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(2); HT{ <max> w: 2 }]) -> 3
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(3); HT{ <max> w: 3 }]) -> 4
      )#");
    } else {
      AssertDocDbDebugDumpStrEq(R"#(
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(1); HT<max>]) -> 2
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(2); HT{ <max> w: 1 }]) -> 3
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(3); HT{ <max> w: 2 }]) -> 4
      )#");
    }
  }

  void RunTestQLInsertUpdate(QLWriteRequestPB_QLStmtType stmt_type, const int ttl = -1) {
    yb::QLWriteRequestPB ql_writereq_pb;
    yb::QLResponsePB ql_writeresp_pb;

    // Define the schema.
    Schema schema = CreateSchema();

    ql_writereq_pb.set_type(stmt_type);
    // Add primary key column.
    AddPrimaryKeyColumn(&ql_writereq_pb, 1);
    ql_writereq_pb.set_hash_code(0);

    AddColumnValues(schema, {2, 3, 4}, &ql_writereq_pb);

    if (ttl != -1) {
      ql_writereq_pb.set_ttl(ttl);
    }

    // Write to docdb.
    WriteQL(&ql_writereq_pb, schema, &ql_writeresp_pb);

    if (ttl == -1) {
      AssertWithoutTTL(stmt_type);
    } else {
      AssertWithTTL(stmt_type);
    }
  }

  yb::QLWriteRequestPB WriteQLRowReq(QLWriteRequestPB_QLStmtType stmt_type, const Schema& schema,
                  const vector<int32_t>& column_values, const HybridTime& hybrid_time,
                  const TransactionOperationContextOpt& txn_op_content =
                      kNonTransactionalOperationContext) {
    yb::QLWriteRequestPB ql_writereq_pb;
    ql_writereq_pb.set_type(stmt_type);

    // Add primary key column.
    ql_writereq_pb.set_hash_code(0);
    for (size_t i = 0; i != schema.num_key_columns(); ++i) {
      if (i < schema.num_hash_key_columns()) {
        AddPrimaryKeyColumn(&ql_writereq_pb, column_values[i]);
      } else {
        AddRangeKeyColumn(column_values[i], &ql_writereq_pb);
      }
    }
    std::vector<int32_t> values(column_values.begin() + schema.num_key_columns(),
                                column_values.end());
    AddColumnValues(schema, values, &ql_writereq_pb);
    return ql_writereq_pb;
  }

  void WriteQLRow(QLWriteRequestPB_QLStmtType stmt_type, const Schema& schema,
                  const vector<int32_t>& column_values, int64_t ttl, const HybridTime& hybrid_time,
                  const TransactionOperationContextOpt& txn_op_content =
                      kNonTransactionalOperationContext) {
    yb::QLWriteRequestPB ql_writereq_pb = WriteQLRowReq(
        stmt_type, schema, column_values, hybrid_time, txn_op_content);
    ql_writereq_pb.set_ttl(ttl);
    yb::QLResponsePB ql_writeresp_pb;
    // Write to docdb.
    WriteQL(&ql_writereq_pb, schema, &ql_writeresp_pb, hybrid_time, txn_op_content);
  }

  void WriteQLRow(QLWriteRequestPB_QLStmtType stmt_type, const Schema& schema,
                  const vector<int32_t>& column_values, const HybridTime& hybrid_time,
                  const TransactionOperationContextOpt& txn_op_content =
                      kNonTransactionalOperationContext) {
    yb::QLWriteRequestPB ql_writereq_pb = WriteQLRowReq(
        stmt_type, schema, column_values, hybrid_time, txn_op_content);
    yb::QLResponsePB ql_writeresp_pb;
    // Write to docdb.
    WriteQL(&ql_writereq_pb, schema, &ql_writeresp_pb, hybrid_time, txn_op_content);
  }

  QLRowBlock ReadQLRow(const Schema& schema, int32_t primary_key, const HybridTime& read_time) {
    QLReadRequestPB ql_read_req;
    ql_read_req.add_hashed_column_values()->mutable_value()->set_int32_value(primary_key);
    ql_read_req.set_hash_code(kFixedHashCode);
    ql_read_req.set_max_hash_code(kFixedHashCode);

    QLRowBlock row_block(schema, vector<ColumnId> ({ColumnId(0), ColumnId(1), ColumnId(2),
                                                        ColumnId(3)}));
    const Schema& projection = row_block.schema();
    QLRSRowDescPB *rsrow_desc_pb = ql_read_req.mutable_rsrow_desc();
    for (int32_t i = 0; i <= 3 ; i++) {
      ql_read_req.add_selected_exprs()->set_column_id(i);
      ql_read_req.mutable_column_refs()->add_ids(i);

      auto col = projection.column_by_id(ColumnId(i));
      EXPECT_OK(col);
      QLRSColDescPB *rscol_desc = rsrow_desc_pb->add_rscol_descs();
      rscol_desc->set_name(col->name());
      col->type()->ToQLTypePB(rscol_desc->mutable_ql_type());
    }

    QLReadOperation read_op(ql_read_req, kNonTransactionalOperationContext);
    QLRocksDBStorage ql_storage(doc_db());
    const QLRSRowDesc rsrow_desc(*rsrow_desc_pb);
    faststring rows_data;
    QLResultSet resultset(&rsrow_desc, &rows_data);
    HybridTime read_restart_ht;
    EXPECT_OK(read_op.Execute(
        ql_storage, CoarseTimePoint::max() /* deadline */, ReadHybridTime::SingleTime(read_time),
        schema, projection, &resultset, &read_restart_ht));
    EXPECT_FALSE(read_restart_ht.is_valid());

    // Transfer the column values from result set to rowblock.
    Slice data(rows_data.data(), rows_data.size());
    EXPECT_OK(row_block.Deserialize(YQL_CLIENT_CQL, &data));
    return row_block;
  }
};

TEST_F(DocOperationTest, TestRedisSetKVWithTTL) {
  // Write key with ttl to docdb.
  auto db = rocksdb();
  yb::RedisWriteRequestPB redis_write_operation_pb;
  auto set_request_pb = redis_write_operation_pb.mutable_set_request();
  set_request_pb->set_ttl(2000);
  redis_write_operation_pb.mutable_key_value()->set_key("abc");
  redis_write_operation_pb.mutable_key_value()->set_type(REDIS_TYPE_STRING);
  redis_write_operation_pb.mutable_key_value()->set_hash_code(123);
  redis_write_operation_pb.mutable_key_value()->add_value("xyz");
  RedisWriteOperation redis_write_operation(&redis_write_operation_pb);
  auto doc_write_batch = MakeDocWriteBatch();
  ASSERT_OK(redis_write_operation.Apply(
      {&doc_write_batch, CoarseTimePoint::max() /* deadline */, ReadHybridTime()}));

  ASSERT_OK(WriteToRocksDB(doc_write_batch, HybridTime::FromMicros(1000)));

  // Read key from rocksdb.
  const KeyBytes doc_key = DocKey::FromRedisKey(123, "abc").Encode();
  rocksdb::ReadOptions read_opts;
  auto iter = std::unique_ptr<rocksdb::Iterator>(db->NewIterator(read_opts));
  ROCKSDB_SEEK(iter.get(), doc_key.AsSlice());
  ASSERT_TRUE(iter->Valid());

  // Verify correct ttl.
  MonoDelta ttl;
  auto value = iter->value();
  ASSERT_OK(Value::DecodeTTL(&value, &ttl));
  EXPECT_EQ(2000, ttl.ToMilliseconds());
}

TEST_F(DocOperationTest, TestQLInsertWithTTL) {
  RunTestQLInsertUpdate(QLWriteRequestPB_QLStmtType_QL_STMT_INSERT, 2000);
}

TEST_F(DocOperationTest, TestQLUpdateWithTTL) {
  RunTestQLInsertUpdate(QLWriteRequestPB_QLStmtType_QL_STMT_UPDATE, 2000);
}

TEST_F(DocOperationTest, TestQLInsertWithoutTTL) {
  RunTestQLInsertUpdate(QLWriteRequestPB_QLStmtType_QL_STMT_INSERT);
}

TEST_F(DocOperationTest, TestQLUpdateWithoutTTL) {
  RunTestQLInsertUpdate(QLWriteRequestPB_QLStmtType_QL_STMT_UPDATE);
}

TEST_F(DocOperationTest, TestQLWriteNulls) {
  yb::QLWriteRequestPB ql_writereq_pb;
  yb::QLResponsePB ql_writeresp_pb;

  // Define the schema.
  Schema schema = CreateSchema();
  ql_writereq_pb.set_type(
      QLWriteRequestPB_QLStmtType::QLWriteRequestPB_QLStmtType_QL_STMT_INSERT);
  ql_writereq_pb.set_hash_code(0);

  // Add primary key column.
  AddPrimaryKeyColumn(&ql_writereq_pb, 1);

  // Add null columns.
  for (int i = 0; i < 3; i++) {
    auto column = ql_writereq_pb.add_column_values();
    column->set_column_id(i + 1);
    column->mutable_expr()->mutable_value();
  }

  // Write to docdb.
  WriteQL(&ql_writereq_pb, schema, &ql_writeresp_pb);

  // Null columns are converted to tombstones.
  AssertDocDbDebugDumpStrEq(R"#(
SubDocKey(DocKey(0x0000, [1], []), [SystemColumnId(0); HT<max>]) -> null
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(1); HT{ <max> w: 1 }]) -> DEL
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(2); HT{ <max> w: 2 }]) -> DEL
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(3); HT{ <max> w: 3 }]) -> DEL
      )#");
}

TEST_F(DocOperationTest, TestQLReadWriteSimple) {
  yb::QLWriteRequestPB ql_writereq_pb;
  yb::QLResponsePB ql_writeresp_pb;

  // Define the schema.
  Schema schema = CreateSchema();
  WriteQLRow(QLWriteRequestPB_QLStmtType_QL_STMT_INSERT, schema, vector<int>({1, 1, 2, 3}),
           1000, HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1000, 0));

  AssertDocDbDebugDumpStrEq(R"#(
SubDocKey(DocKey(0x0000, [1], []), [SystemColumnId(0); HT{ physical: 1000 }]) -> null; ttl: 1.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(1); HT{ physical: 1000 w: 1 }]) -> 1; ttl: 1.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(2); HT{ physical: 1000 w: 2 }]) -> 2; ttl: 1.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(3); HT{ physical: 1000 w: 3 }]) -> 3; ttl: 1.000s
      )#");

  // Now read the value.
  QLRowBlock row_block = ReadQLRow(schema, 1,
                                  HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(2000, 0));
  ASSERT_EQ(1, row_block.row_count());
  EXPECT_EQ(1, row_block.row(0).column(0).int32_value());
  EXPECT_EQ(1, row_block.row(0).column(1).int32_value());
  EXPECT_EQ(2, row_block.row(0).column(2).int32_value());
  EXPECT_EQ(3, row_block.row(0).column(3).int32_value());
}

TEST_F(DocOperationTest, TestQLRangeDeleteWithStaticColumnAvoidsFullPartitionKeyScan) {
  constexpr int kNumRows = 1000;
  constexpr int kDeleteRangeLow = 100;
  constexpr int kDeleteRangeHigh = 200;

  // Define the schema with a partition key, range key, static column, and value.
  SchemaBuilder builder;
  builder.set_next_column_id(ColumnId(0));
  ASSERT_OK(builder.AddHashKeyColumn("k", INT32));
  ASSERT_OK(builder.AddKeyColumn("r", INT32));
  ASSERT_OK(builder.AddColumn(ColumnSchema("s", INT32, false, false, true), false));
  ASSERT_OK(builder.AddColumn(ColumnSchema("v", INT32), false));
  auto schema = builder.Build();

  // Write rows with the same partition key but different range key
  for (int row_num = 0; row_num < kNumRows; ++row_num) {
    WriteQLRow(
        QLWriteRequestPB_QLStmtType_QL_STMT_INSERT,
        schema,
        vector<int>({1, row_num, 0, row_num}),
        HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1000, 0));
  }
  auto get_num_visited_rows = [&]() {
    auto num_next = regular_db_options().statistics->getTickerCount(
      rocksdb::Tickers::NUMBER_DB_NEXT);
    auto num_seek = regular_db_options().statistics->getTickerCount(
      rocksdb::Tickers::NUMBER_DB_SEEK);
    return num_next + num_seek;
  };

  auto visited_rows_before_delete = get_num_visited_rows();

  // Delete a subset of the partition
  yb::QLWriteRequestPB ql_writereq_pb;
  yb::QLResponsePB ql_writeresp_pb;
  ql_writereq_pb.set_type(QLWriteRequestPB_QLStmtType_QL_STMT_DELETE);
  AddPrimaryKeyColumn(&ql_writereq_pb, 1);

  auto where_clause_and = ql_writereq_pb.mutable_where_expr()->mutable_condition();
  where_clause_and->set_op(QLOperator::QL_OP_AND);
  QLAddInt32Condition(where_clause_and, 1, QL_OP_GREATER_THAN_EQUAL, kDeleteRangeLow);
  QLAddInt32Condition(where_clause_and, 1, QL_OP_LESS_THAN_EQUAL, kDeleteRangeHigh);
  ql_writereq_pb.set_hash_code(0);

  WriteQL(&ql_writereq_pb, schema, &ql_writeresp_pb,
          HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(2000, 0),
          kNonTransactionalOperationContext);

  // During deletion, we expect to visit each docdb row in range plus the first row out of range. In
  // this case, there is a docdb row for the liveness column and a docdb row for the value for each
  // cql row.
  auto num_cql_rows_in_range = kDeleteRangeHigh - kDeleteRangeLow + 1;
  auto num_docdb_rows_per_cql_row = 2;
  auto num_docdb_rows_in_range = num_cql_rows_in_range * num_docdb_rows_per_cql_row;
  ASSERT_EQ(
      get_num_visited_rows() - visited_rows_before_delete,
      num_docdb_rows_in_range + 1);
}

TEST_F(DocOperationTest, TestQLReadWithoutLivenessColumn) {
  const DocKey doc_key(kFixedHashCode, PrimitiveValues(PrimitiveValue::Int32(100)),
                       PrimitiveValues());
  KeyBytes encoded_doc_key(doc_key.Encode());
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(ColumnId(1))),
                         Value(PrimitiveValue::Int32(2)), HybridTime(1000)));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(ColumnId(2))),
                         Value(PrimitiveValue::Int32(3)), HybridTime(2000)));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(ColumnId(3))),
                         Value(PrimitiveValue::Int32(4)), HybridTime(3000)));

  AssertDocDbDebugDumpStrEq(R"#(
SubDocKey(DocKey(0x0000, [100], []), [ColumnId(1); HT{ physical: 0 logical: 1000 }]) -> 2
SubDocKey(DocKey(0x0000, [100], []), [ColumnId(2); HT{ physical: 0 logical: 2000 }]) -> 3
SubDocKey(DocKey(0x0000, [100], []), [ColumnId(3); HT{ physical: 0 logical: 3000 }]) -> 4
      )#");

  // Now verify we can read without the system column id.
  Schema schema = CreateSchema();
  HybridTime read_time(3000);
  QLRowBlock row_block = ReadQLRow(schema, 100, read_time);
  ASSERT_EQ(1, row_block.row_count());
  EXPECT_EQ(100, row_block.row(0).column(0).int32_value());
  EXPECT_EQ(2, row_block.row(0).column(1).int32_value());
  EXPECT_EQ(3, row_block.row(0).column(2).int32_value());
  EXPECT_EQ(4, row_block.row(0).column(3).int32_value());
}

TEST_F(DocOperationTest, TestQLReadWithTombstone) {
  DocKey doc_key(0, PrimitiveValues(PrimitiveValue::Int32(100)), PrimitiveValues());
  KeyBytes encoded_doc_key(doc_key.Encode());
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(ColumnId(1))),
                         Value(PrimitiveValue::kTombstone), HybridTime(1000)));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(ColumnId(2))),
                         Value(PrimitiveValue::kTombstone), HybridTime(2000)));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(ColumnId(3))),
                         Value(PrimitiveValue::kTombstone), HybridTime(3000)));

  AssertDocDbDebugDumpStrEq(R"#(
SubDocKey(DocKey(0x0000, [100], []), [ColumnId(1); HT{ physical: 0 logical: 1000 }]) -> DEL
SubDocKey(DocKey(0x0000, [100], []), [ColumnId(2); HT{ physical: 0 logical: 2000 }]) -> DEL
SubDocKey(DocKey(0x0000, [100], []), [ColumnId(3); HT{ physical: 0 logical: 3000 }]) -> DEL
      )#");

  Schema schema = CreateSchema();
  DocRowwiseIterator iter(schema, schema, kNonTransactionalOperationContext,
                          doc_db(), CoarseTimePoint::max() /* deadline */,
                          ReadHybridTime::FromUint64(3000));
  ASSERT_OK(iter.Init());
  ASSERT_FALSE(ASSERT_RESULT(iter.HasNext()));

  // Now verify row exists even with one valid column.
  doc_key = DocKey(kFixedHashCode, PrimitiveValues(PrimitiveValue::Int32(100)), PrimitiveValues());
  encoded_doc_key = doc_key.Encode();
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(ColumnId(1))),
                         Value(PrimitiveValue::kTombstone), HybridTime(1001)));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(ColumnId(2))),
                         Value(PrimitiveValue::Int32(2),
                               MonoDelta::FromMilliseconds(1)), HybridTime(2001)));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(ColumnId(3))),
                         Value(PrimitiveValue::Int32(101)), HybridTime(3001)));

  AssertDocDbDebugDumpStrEq(R"#(
SubDocKey(DocKey(0x0000, [100], []), [ColumnId(1); HT{ physical: 0 logical: 1001 }]) -> DEL
SubDocKey(DocKey(0x0000, [100], []), [ColumnId(1); HT{ physical: 0 logical: 1000 }]) -> DEL
SubDocKey(DocKey(0x0000, [100], []), [ColumnId(2); HT{ physical: 0 logical: 2001 }]) -> \
    2; ttl: 0.001s
SubDocKey(DocKey(0x0000, [100], []), [ColumnId(2); HT{ physical: 0 logical: 2000 }]) -> DEL
SubDocKey(DocKey(0x0000, [100], []), [ColumnId(3); HT{ physical: 0 logical: 3001 }]) -> 101
SubDocKey(DocKey(0x0000, [100], []), [ColumnId(3); HT{ physical: 0 logical: 3000 }]) -> DEL
      )#");

  vector<PrimitiveValue> hashed_components({PrimitiveValue::Int32(100)});
  DocQLScanSpec ql_scan_spec(schema, kFixedHashCode, kFixedHashCode, hashed_components,
      /* req */ nullptr, /* if_req */ nullptr, rocksdb::kDefaultQueryId);

  DocRowwiseIterator ql_iter(
      schema, schema, kNonTransactionalOperationContext, doc_db(),
      CoarseTimePoint::max() /* deadline */, ReadHybridTime::FromMicros(3000));
  ASSERT_OK(ql_iter.Init(ql_scan_spec));
  ASSERT_TRUE(ASSERT_RESULT(ql_iter.HasNext()));
  QLTableRow value_map;
  ASSERT_OK(ql_iter.NextRow(&value_map));
  ASSERT_EQ(4, value_map.ColumnCount());
  EXPECT_EQ(100, value_map.TestValue(0).value.int32_value());
  EXPECT_TRUE(IsNull(value_map.TestValue(1).value));
  EXPECT_TRUE(IsNull(value_map.TestValue(2).value));
  EXPECT_EQ(101, value_map.TestValue(3).value.int32_value());

  // Now verify row exists as long as liveness system column exists.
  doc_key = DocKey(kFixedHashCode, PrimitiveValues(PrimitiveValue::Int32(101)), PrimitiveValues());
  encoded_doc_key = doc_key.Encode();
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key,
                                 PrimitiveValue::SystemColumnId(
                                     SystemColumnIds::kLivenessColumn)),
                         Value(PrimitiveValue(ValueType::kNullLow)),
                         HybridTime(1000)));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(ColumnId(1))),
                         Value(PrimitiveValue::kTombstone), HybridTime(1000)));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(ColumnId(2))),
                         Value(PrimitiveValue::Int32(2),
                               MonoDelta::FromMilliseconds(1)), HybridTime(2000)));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(ColumnId(3))),
                         Value(PrimitiveValue::kTombstone), HybridTime(3000)));

  AssertDocDbDebugDumpStrEq(R"#(
SubDocKey(DocKey(0x0000, [100], []), [ColumnId(1); HT{ physical: 0 logical: 1001 }]) -> DEL
SubDocKey(DocKey(0x0000, [100], []), [ColumnId(1); HT{ physical: 0 logical: 1000 }]) -> DEL
SubDocKey(DocKey(0x0000, [100], []), [ColumnId(2); HT{ physical: 0 logical: 2001 }]) -> \
    2; ttl: 0.001s
SubDocKey(DocKey(0x0000, [100], []), [ColumnId(2); HT{ physical: 0 logical: 2000 }]) -> DEL
SubDocKey(DocKey(0x0000, [100], []), [ColumnId(3); HT{ physical: 0 logical: 3001 }]) -> 101
SubDocKey(DocKey(0x0000, [100], []), [ColumnId(3); HT{ physical: 0 logical: 3000 }]) -> DEL
SubDocKey(DocKey(0x0000, [101], []), [SystemColumnId(0); HT{ physical: 0 logical: 1000 }]) -> null
SubDocKey(DocKey(0x0000, [101], []), [ColumnId(1); HT{ physical: 0 logical: 1000 }]) -> DEL
SubDocKey(DocKey(0x0000, [101], []), [ColumnId(2); HT{ physical: 0 logical: 2000 }]) -> \
    2; ttl: 0.001s
SubDocKey(DocKey(0x0000, [101], []), [ColumnId(3); HT{ physical: 0 logical: 3000 }]) -> DEL
      )#");

  vector<PrimitiveValue> hashed_components_system({PrimitiveValue::Int32(101)});
  DocQLScanSpec ql_scan_spec_system(schema, kFixedHashCode, kFixedHashCode,
      hashed_components_system, /* req */ nullptr,  /* if_req */ nullptr,
      rocksdb::kDefaultQueryId);

  DocRowwiseIterator ql_iter_system(
      schema, schema, kNonTransactionalOperationContext, doc_db(),
      CoarseTimePoint::max() /* deadline */, ReadHybridTime::FromMicros(3000));
  ASSERT_OK(ql_iter_system.Init(ql_scan_spec_system));
  ASSERT_TRUE(ASSERT_RESULT(ql_iter_system.HasNext()));
  QLTableRow value_map_system;
  ASSERT_OK(ql_iter_system.NextRow(&value_map_system));
  ASSERT_EQ(4, value_map_system.ColumnCount());
  EXPECT_EQ(101, value_map_system.TestValue(0).value.int32_value());
  EXPECT_TRUE(IsNull(value_map_system.TestValue(1).value));
  EXPECT_TRUE(IsNull(value_map_system.TestValue(2).value));
  EXPECT_TRUE(IsNull(value_map_system.TestValue(3).value));
}

namespace {

int32_t NewInt(std::mt19937_64* rng, std::unordered_set<int32_t>* existing) {
  std::uniform_int_distribution<uint32_t> distribution;
  for (;;) {
    auto result = distribution(*rng);
    if (existing->insert(static_cast<int32_t>(result)).second) {
      return result;
    }
  }
}

int32_t NewInt(std::mt19937_64* rng, std::unordered_set<int32_t>* existing,
    const int32_t min, const int32_t max) {
  std::uniform_int_distribution<int32_t> distribution(min, max);
  for (;;) {
    auto result = distribution(*rng);
    if (existing->insert(result).second) {
      return result;
    }
  }
}

struct RowData {
  int32_t k;
  int32_t r;
  int32_t v;
};

struct RowDataWithHt {
  RowData data;
  HybridTime ht;
};

bool operator==(const RowData& lhs, const RowData& rhs) {
  return lhs.k == rhs.k && lhs.r == rhs.r && lhs.v == rhs.v;
}

bool operator<(const RowData& lhs, const RowData& rhs) {
  return lhs.k == rhs.k ? (lhs.r == rhs.r ? lhs.v < rhs.v : lhs.r < rhs.r) : lhs.k < rhs.k;
}

bool IsSameKey(const RowData& lhs, const RowData& rhs) {
  return lhs.k == rhs.k && lhs.r == rhs.r;
}

bool IsKeyLessThan(const RowData& lhs, const RowData& rhs) {
  return lhs.k == rhs.k ? lhs.r < rhs.r : lhs.k < rhs.k;
}

std::ostream& operator<<(std::ostream& out, const RowData& row) {
  return out << "{ k: " << row.k << " r: " << row.r << " v: " << row.v << " }";
}

bool IsSameKey(const RowDataWithHt& lhs, const RowDataWithHt& rhs) {
  return IsSameKey(lhs.data, rhs.data);
}

bool IsKeyLessThan(const RowDataWithHt& lhs, const RowDataWithHt& rhs) {
  return IsKeyLessThan(lhs.data, rhs.data);
}

bool operator<(const RowDataWithHt& lhs, const RowDataWithHt& rhs) {
  return IsSameKey(lhs, rhs)
      ? (lhs.ht == rhs.ht ? lhs.data < rhs.data : lhs.ht > rhs.ht)
      : IsKeyLessThan(lhs, rhs);
}

std::ostream& operator<<(std::ostream& out, const RowDataWithHt& row) {
  return out << "{ data: " << row.data << " ht: " << row.ht << " }";
}

template<class It>
It MoveForwardToNextKey(It it, const It end) {
  return std::find_if_not(it, end, [it](auto k) { return IsSameKey(k, *it); });
}

template<class It>
It MoveBackToBeginningOfCurrentKey(const It begin, It it) {

  return std::lower_bound(begin, it, *it, [](auto k1, auto k2) { return IsKeyLessThan(k1, k2); });
}

template<class It>
std::pair<It, It> GetIteratorRange(const It begin, const It end, const It it, QLOperator op) {
  switch (op) {
    case QL_OP_EQUAL:
      {
        return std::make_pair(MoveBackToBeginningOfCurrentKey(begin, it),
            MoveForwardToNextKey(it, end));
      }
    case QL_OP_LESS_THAN:
      {
        return std::make_pair(begin, MoveBackToBeginningOfCurrentKey(begin, it));
      }
    case QL_OP_LESS_THAN_EQUAL:
      {
        return std::make_pair(begin, MoveForwardToNextKey(it, end));
      }
    case QL_OP_GREATER_THAN:
      {
        return std::make_pair(MoveForwardToNextKey(it, end), end);
      }
    case QL_OP_GREATER_THAN_EQUAL:
      {
        return std::make_pair(MoveBackToBeginningOfCurrentKey(begin, it), end);
      }
    default: // We should not handle all cases here
      LOG(FATAL) << "Unexpected op: " << QLOperator_Name(op);
      return std::make_pair(begin, end);
  }
}

} // namespace

class DocOperationScanTest : public DocOperationTest {
 protected:
  DocOperationScanTest() {
    Seed(&rng_);
  }

  void InitSchema(ColumnSchema::SortingType range_column_sorting) {
    range_column_sorting_type_ = range_column_sorting;
    ColumnSchema hash_column("k", INT32, false, true);
    ColumnSchema range_column("r", INT32, false, false, false, false, 1, range_column_sorting);
    ColumnSchema value_column("v", INT32, false, false);
    auto columns = { hash_column, range_column, value_column };
    schema_ = Schema(columns, CreateColumnIds(columns.size()), 2);
  }

  void InsertRows(const size_t num_rows_per_key,
      TransactionStatusManagerMock* txn_status_manager = nullptr) {
    ResetCurrentTransactionId();
    ASSERT_OK(DisableCompactions());

    std::unordered_set<int32_t> used_ints;
    h_key_ = NewInt(&rng_, &used_ints);
    rows_.clear();
    for (int32_t i = 0; i != kNumKeys; ++i) {
      int32_t r_key = NewInt(&rng_, &used_ints);
      for (int32_t j = 0; j < num_rows_per_key; ++j) {
        RowData row_data = {h_key_, r_key, NewInt(&rng_, &used_ints)};
        auto ht = HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(
            NewInt(&rng_, &used_ints, kMinTime, kMaxTime), 0);
        RowDataWithHt row = {row_data, ht};
        rows_.push_back(row);

        std::unique_ptr<TransactionOperationContext> txn_op_context;
        boost::optional<TransactionId> txn_id;
        if (txn_status_manager) {
          if (RandomActWithProbability(0.5, &rng_)) {
            txn_id = TransactionId::GenerateRandom();
            SetCurrentTransactionId(*txn_id);
            txn_op_context = std::make_unique<TransactionOperationContext>(*txn_id,
                txn_status_manager);
          } else {
            ResetCurrentTransactionId();
          }
        }
        WriteQLRow(QLWriteRequestPB_QLStmtType_QL_STMT_INSERT,
                   schema_,
                   { row_data.k, row_data.r, row_data.v },
                   1000,
                   ht,
                   txn_op_context ? *txn_op_context : kNonTransactionalOperationContext);
        if (txn_id) {
          txn_status_manager->Commit(*txn_id, ht);
        }
      }

      ASSERT_OK(FlushRocksDbAndWait());
    }

    DumpRocksDBToLog(rocksdb(), StorageDbType::kRegular);
    DumpRocksDBToLog(intents_db(), StorageDbType::kIntents);
  }

  void PerformScans(const bool is_forward_scan,
      const TransactionOperationContextOpt& txn_op_context,
      boost::function<void(const size_t keys_in_scan_range)> after_scan_callback) {
    std::vector <PrimitiveValue> hashed_components = {PrimitiveValue::Int32(h_key_)};
    std::vector <QLOperator> operators = {
        QL_OP_EQUAL,
        QL_OP_LESS_THAN_EQUAL,
        QL_OP_GREATER_THAN_EQUAL,
    };

    std::shuffle(rows_.begin(), rows_.end(), rng_);
    auto ordered_rows = rows_;
    std::sort(ordered_rows.begin(), ordered_rows.end());

    for (const auto op : operators) {
      LOG(INFO) << "Testing: " << QLOperator_Name(op);
      for (const auto& row : rows_) {
        QLConditionPB condition;
        condition.add_operands()->set_column_id(1_ColId);
        condition.set_op(op);
        condition.add_operands()->mutable_value()->set_int32_value(row.data.r);
        auto it = std::lower_bound(ordered_rows.begin(), ordered_rows.end(), row);
        LOG(INFO) << "Bound: " << yb::ToString(*it);
        const auto range = GetIteratorRange(ordered_rows.begin(), ordered_rows.end(), it, op);
        std::vector <int32_t> ht_diffs;
        if (op == QL_OP_EQUAL) {
          ht_diffs = {0};
        } else {
          ht_diffs = {-1, 0, 1};
        }
        for (auto ht_diff : ht_diffs) {
          std::vector <RowDataWithHt> expected_rows;
          auto read_ht = ReadHybridTime::FromMicros(row.ht.GetPhysicalValueMicros() + ht_diff);
          LOG(INFO) << "Read time: " << read_ht;
          size_t keys_in_range = range.first < range.second;
          for (auto it_r = range.first; it_r < range.second;) {
            auto it = it_r;
            if (it_r->ht <= read_ht.read) {
              expected_rows.emplace_back(*it_r);
              while (it_r < range.second && IsSameKey(*it_r, *it)) {
                ++it_r;
              }
            } else {
              ++it_r;
            }
            if (it_r < range.second && !IsSameKey(*it_r, *it)) {
              ++keys_in_range;
            }
          }

          if (is_forward_scan ==
              (range_column_sorting_type_ == ColumnSchema::SortingType::kDescending)) {
            std::reverse(expected_rows.begin(), expected_rows.end());
          }
          DocQLScanSpec ql_scan_spec(
              schema_, kFixedHashCode, kFixedHashCode, hashed_components,
              &condition, nullptr /* if_ req */, rocksdb::kDefaultQueryId, is_forward_scan);
          DocRowwiseIterator ql_iter(
              schema_, schema_, txn_op_context, doc_db(), CoarseTimePoint::max() /* deadline */,
              read_ht);
          ASSERT_OK(ql_iter.Init(ql_scan_spec));
          LOG(INFO) << "Expected rows: " << yb::ToString(expected_rows);
          it = expected_rows.begin();
          while (ASSERT_RESULT(ql_iter.HasNext())) {
            QLTableRow value_map;
            ASSERT_OK(ql_iter.NextRow(&value_map));
            ASSERT_EQ(3, value_map.ColumnCount());

            RowData fetched_row = {value_map.TestValue(0_ColId).value.int32_value(),
                value_map.TestValue(1_ColId).value.int32_value(),
                value_map.TestValue(2_ColId).value.int32_value()};
            LOG(INFO) << "Fetched row: " << fetched_row;
            ASSERT_LT(it, expected_rows.end());
            ASSERT_EQ(fetched_row, it->data);
            it++;
          }
          ASSERT_EQ(expected_rows.end(), it);

          after_scan_callback(keys_in_range);
        }
      }
    }
  }

  void TestWithSortingType(ColumnSchema::SortingType sorting_type, bool is_forward_scan) {
    DoTestWithSortingType(sorting_type, is_forward_scan, 1 /* num_rows_per_key */);
    ASSERT_OK(DestroyRocksDB());
    ASSERT_OK(ReopenRocksDB());
    DoTestWithSortingType(sorting_type, is_forward_scan, 5 /* num_rows_per_key */);
  }

  virtual void DoTestWithSortingType(ColumnSchema::SortingType schema_type, bool is_forward_scan,
      size_t num_rows_per_key) = 0;

  constexpr static int32_t kNumKeys = 20;
  constexpr static uint32_t kMinTime = 500;
  constexpr static uint32_t kMaxTime = 1500;

  std::mt19937_64 rng_;
  ColumnSchema::SortingType range_column_sorting_type_;
  Schema schema_;
  int32_t h_key_;
  std::vector<RowDataWithHt> rows_;
};

class DocOperationRangeFilterTest : public DocOperationScanTest {
 protected:
  void DoTestWithSortingType(ColumnSchema::SortingType sorting_type, bool is_forward_scan,
      size_t num_rows_per_key) override;
};

// Currently we test using one column and one scan type.
// TODO(akashnil): In future we want to implement and test arbitrary ASC DESC combinations for scan.
void DocOperationRangeFilterTest::DoTestWithSortingType(ColumnSchema::SortingType sorting_type,
    const bool is_forward_scan, const size_t num_rows_per_key) {
  ASSERT_OK(DisableCompactions());

  InitSchema(sorting_type);
  InsertRows(num_rows_per_key);

  {
    std::vector<rocksdb::LiveFileMetaData> live_files;
    rocksdb()->GetLiveFilesMetaData(&live_files);
    const auto expected_live_files = kNumKeys;
    ASSERT_EQ(expected_live_files, live_files.size());
  }

  auto old_iterators =
      rocksdb()->GetDBOptions().statistics->getTickerCount(rocksdb::NO_TABLE_CACHE_ITERATORS);

  PerformScans(is_forward_scan, kNonTransactionalOperationContext,
      [this, &old_iterators](const size_t key_in_scan_range) {
    auto new_iterators =
        rocksdb()->GetDBOptions().statistics->getTickerCount(rocksdb::NO_TABLE_CACHE_ITERATORS);
    ASSERT_EQ(key_in_scan_range, new_iterators - old_iterators);
    old_iterators = new_iterators;
  });
}

TEST_F_EX(DocOperationTest, QLRangeFilterAscendingForwardScan, DocOperationRangeFilterTest) {
  TestWithSortingType(ColumnSchema::kAscending, true);
}

TEST_F_EX(DocOperationTest, QLRangeFilterDescendingForwardScan, DocOperationRangeFilterTest) {
  TestWithSortingType(ColumnSchema::kDescending, true);
}

TEST_F_EX(DocOperationTest, QLRangeFilterAscendingReverseScan, DocOperationRangeFilterTest) {
  TestWithSortingType(ColumnSchema::kAscending, false);
}

TEST_F_EX(DocOperationTest, QLRangeFilterDescendingReverseScan, DocOperationRangeFilterTest) {
  TestWithSortingType(ColumnSchema::kDescending, false);
}

class DocOperationTxnScanTest : public DocOperationScanTest {
 protected:
  void DoTestWithSortingType(ColumnSchema::SortingType sorting_type, bool is_forward_scan,
      size_t num_rows_per_key) override {
    ASSERT_OK(DisableCompactions());

    InitSchema(sorting_type);

    TransactionStatusManagerMock txn_status_manager;
    SetTransactionIsolationLevel(IsolationLevel::SNAPSHOT_ISOLATION);

    InsertRows(num_rows_per_key, &txn_status_manager);

    PerformScans(is_forward_scan,
                 TransactionOperationContext(TransactionId::GenerateRandom(), &txn_status_manager),
                 [](size_t){});
  }
};

TEST_F_EX(DocOperationTest, QLTxnAscendingForwardScan, DocOperationTxnScanTest) {
  TestWithSortingType(ColumnSchema::kAscending, true);
}

TEST_F_EX(DocOperationTest, QLTxnDescendingForwardScan, DocOperationTxnScanTest) {
  TestWithSortingType(ColumnSchema::kDescending, true);
}

TEST_F_EX(DocOperationTest, QLTxnAscendingReverseScan, DocOperationTxnScanTest) {
  TestWithSortingType(ColumnSchema::kAscending, false);
}

TEST_F_EX(DocOperationTest, QLTxnDescendingReverseScan, DocOperationTxnScanTest) {
  TestWithSortingType(ColumnSchema::kDescending, false);
}

TEST_F(DocOperationTest, TestQLCompactions) {
  yb::QLWriteRequestPB ql_writereq_pb;
  yb::QLResponsePB ql_writeresp_pb;

  HybridTime t0 = HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1000, 0);
  HybridTime t0prime = HybridClock::HybridTimeFromMicrosecondsAndLogicalValue(1000, 1);
  HybridTime t1 = HybridClock::AddPhysicalTimeToHybridTime(t0, MonoDelta::FromMilliseconds(1001));

  // Define the schema.
  Schema schema = CreateSchema();
  WriteQLRow(QLWriteRequestPB_QLStmtType_QL_STMT_INSERT, schema, vector<int>({1, 1, 2, 3}),
      1000, t0);

  AssertDocDbDebugDumpStrEq(R"#(
SubDocKey(DocKey(0x0000, [1], []), [SystemColumnId(0); HT{ physical: 1000 }]) -> null; ttl: 1.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(1); HT{ physical: 1000 w: 1 }]) -> 1; ttl: 1.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(2); HT{ physical: 1000 w: 2 }]) -> 2; ttl: 1.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(3); HT{ physical: 1000 w: 3 }]) -> 3; ttl: 1.000s
      )#");

  FullyCompactHistoryBefore(t1);

  // Verify all entries are purged.
  AssertDocDbDebugDumpStrEq(R"#(
      )#");

  // Add a row with a TTL.
  WriteQLRow(QLWriteRequestPB_QLStmtType_QL_STMT_INSERT, schema, vector<int>({1, 1, 2, 3}),
      1000, t0);
  AssertDocDbDebugDumpStrEq(R"#(
SubDocKey(DocKey(0x0000, [1], []), [SystemColumnId(0); HT{ physical: 1000 }]) -> null; ttl: 1.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(1); HT{ physical: 1000 w: 1 }]) -> 1; ttl: 1.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(2); HT{ physical: 1000 w: 2 }]) -> 2; ttl: 1.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(3); HT{ physical: 1000 w: 3 }]) -> 3; ttl: 1.000s
     )#");

  // Update the columns with a higher TTL.
  yb::QLWriteRequestPB ql_update_pb;
  yb::QLResponsePB ql_update_resp_pb;
  ql_writereq_pb.set_type(
      QLWriteRequestPB_QLStmtType::QLWriteRequestPB_QLStmtType_QL_STMT_UPDATE);

  ql_writereq_pb.set_hash_code(kFixedHashCode);
  AddPrimaryKeyColumn(&ql_writereq_pb, 1);
  AddColumnValues(schema, {10, 20, 30}, &ql_writereq_pb);
  ql_writereq_pb.set_ttl(2000);

  // Write to docdb at the same physical time and a bumped-up logical time.
  WriteQL(&ql_writereq_pb, schema, &ql_writeresp_pb, t0prime);

  AssertDocDbDebugDumpStrEq(R"#(
SubDocKey(DocKey(0x0000, [1], []), [SystemColumnId(0); HT{ physical: 1000 }]) -> \
    null; ttl: 1.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(1); HT{ physical: 1000 logical: 1 }]) -> \
    10; ttl: 2.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(1); HT{ physical: 1000 w: 1 }]) -> \
    1; ttl: 1.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(2); HT{ physical: 1000 logical: 1 w: 1 }]) -> \
    20; ttl: 2.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(2); HT{ physical: 1000 w: 2 }]) -> \
    2; ttl: 1.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(3); HT{ physical: 1000 logical: 1 w: 2 }]) -> \
    30; ttl: 2.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(3); HT{ physical: 1000 w: 3 }]) -> 3; ttl: 1.000s
      )#");

  FullyCompactHistoryBefore(t1);

  // Verify the rest of the columns still live.
  AssertDocDbDebugDumpStrEq(R"#(
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(1); HT{ physical: 1000 logical: 1 }]) -> \
    10; ttl: 2.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(2); HT{ physical: 1000 logical: 1 w: 1 }]) -> \
    20; ttl: 2.000s
SubDocKey(DocKey(0x0000, [1], []), [ColumnId(3); HT{ physical: 1000 logical: 1 w: 2 }]) -> \
    30; ttl: 2.000s
      )#");

  // Verify reads work well without system column id.
  QLRowBlock row_block = ReadQLRow(schema, 1, t1);
  ASSERT_EQ(1, row_block.row_count());
  EXPECT_EQ(1, row_block.row(0).column(0).int32_value());
  EXPECT_EQ(10, row_block.row(0).column(1).int32_value());
  EXPECT_EQ(20, row_block.row(0).column(2).int32_value());
  EXPECT_EQ(30, row_block.row(0).column(3).int32_value());
}

namespace {

size_t GenerateFiles(int total_batches, DocOperationTest* test) {
  auto schema = test->CreateSchema();

  auto t0 = HybridTime::FromMicrosecondsAndLogicalValue(1000, 0);
  const int kBigFileFrequency = 7;
  const int kBigFileRows = 10000;
  size_t expected_files = 0;
  bool first = true;
  for (int i = 0; i != total_batches; ++i) {
    int base = i * kBigFileRows;
    int count;
    if (i % kBigFileFrequency == 0) {
      count = kBigFileRows;
      ++expected_files;
      first = true;
    } else {
      count = 1;
      if (first) {
        ++expected_files;
        first = false;
      }
    }
    for (int j = base; j != base + count; ++j) {
      test->WriteQLRow(
          QLWriteRequestPB_QLStmtType_QL_STMT_INSERT, schema, {j, j, j, j}, 1000000, t0);
    }
    EXPECT_OK(test->FlushRocksDbAndWait());
  }
  return expected_files;
}

void WaitCompactionsDone(rocksdb::DB* db) {
  for (;;) {
    std::this_thread::sleep_for(500ms);
    uint64_t value = 0;
    ASSERT_TRUE(db->GetIntProperty(rocksdb::DB::Properties::kNumRunningCompactions, &value));
    if (value == 0) {
      break;
    }
  }
}

} // namespace

TEST_F(DocOperationTest, MaxFileSizeForCompaction) {
  google::FlagSaver flag_saver;

  ASSERT_OK(DisableCompactions());
  const int kTotalBatches = 20;
  auto expected_files = GenerateFiles(kTotalBatches, this);

  std::vector<rocksdb::LiveFileMetaData> files;
  rocksdb()->GetLiveFilesMetaData(&files);
  ASSERT_EQ(kTotalBatches, files.size());

  FLAGS_rocksdb_max_file_size_for_compaction = 100_KB;
  ASSERT_OK(ReinitDBOptions());

  WaitCompactionsDone(rocksdb());

  files.clear();
  rocksdb()->GetLiveFilesMetaData(&files);
  ASSERT_EQ(expected_files, files.size());
}

TEST_F(DocOperationTest, MaxFileSizeWithWritesTrigger) {
  google::FlagSaver flag_saver;

  ASSERT_OK(DisableCompactions());
  const int kTotalBatches = 20;
  GenerateFiles(kTotalBatches, this);

  FLAGS_rocksdb_max_file_size_for_compaction = 100_KB;
  ASSERT_OK(ReinitDBOptions());

  WaitCompactionsDone(rocksdb());

  FLAGS_rocksdb_level0_slowdown_writes_trigger = 2;
  FLAGS_rocksdb_level0_stop_writes_trigger = 1;
  ASSERT_OK(ReinitDBOptions());

  std::vector<rocksdb::LiveFileMetaData> files;
  rocksdb()->GetLiveFilesMetaData(&files);
  ASSERT_GE(files.size(), 3);

  auto handle_impl = down_cast<rocksdb::ColumnFamilyHandleImpl*>(
      regular_db_->DefaultColumnFamily());
  auto stats = handle_impl->cfd()->internal_stats();
  ASSERT_EQ(0, stats->GetCFStats(rocksdb::InternalStats::LEVEL0_NUM_FILES_TOTAL));
  ASSERT_EQ(0, stats->GetCFStats(rocksdb::InternalStats::LEVEL0_SLOWDOWN_TOTAL));
}

}  // namespace docdb
}  // namespace yb
