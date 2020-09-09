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

#include <memory>
#include <string>

#include "yb/common/ql_expr.h"
#include "yb/common/ql_value.h"
#include "yb/common/transaction-test-util.h"

#include "yb/docdb/doc_rowwise_iterator.h"
#include "yb/docdb/docdb.h"
#include "yb/docdb/docdb_debug.h"
#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/docdb/docdb_test_base.h"
#include "yb/docdb/docdb_test_util.h"
#include "yb/docdb/intent.h"

#include "yb/server/hybrid_clock.h"

#include "yb/util/size_literals.h"
#include "yb/util/test_macros.h"
#include "yb/util/test_util.h"

DECLARE_bool(TEST_docdb_sort_weak_intents_in_tests);

namespace yb {
namespace docdb {

class DocRowwiseIteratorTest : public DocDBTestBase {
 protected:
  DocRowwiseIteratorTest() {
    SeedRandom();
  }
  ~DocRowwiseIteratorTest() override {}

  // TODO Could define them out of class, so one line would be enough for them.
  static const KeyBytes kEncodedDocKey1;
  static const KeyBytes kEncodedDocKey2;
  static const Schema kSchemaForIteratorTests;
  static Schema kProjectionForIteratorTests;

  void SetUp() override {
    FLAGS_TEST_docdb_sort_weak_intents_in_tests = true;
    DocDBTestBase::SetUp();
  }

  static void SetUpTestCase() {
    ASSERT_OK(kSchemaForIteratorTests.CreateProjectionByNames({"c", "d", "e"},
        &kProjectionForIteratorTests));
  }
};

const KeyBytes DocRowwiseIteratorTest::kEncodedDocKey1(
    DocKey(PrimitiveValues("row1", 11111)).Encode());

const KeyBytes DocRowwiseIteratorTest::kEncodedDocKey2(
    DocKey(PrimitiveValues("row2", 22222)).Encode());

const Schema DocRowwiseIteratorTest::kSchemaForIteratorTests({
        ColumnSchema("a", DataType::STRING, /* is_nullable = */ false),
        ColumnSchema("b", DataType::INT64, false),
        // Non-key columns
        ColumnSchema("c", DataType::STRING, true),
        ColumnSchema("d", DataType::INT64, true),
        ColumnSchema("e", DataType::STRING, true)
    }, {
        10_ColId,
        20_ColId,
        30_ColId,
        40_ColId,
        50_ColId
    }, 2);

Schema DocRowwiseIteratorTest::kProjectionForIteratorTests;

TEST_F(DocRowwiseIteratorTest, DocRowwiseIteratorTest) {
  // Row 1
  // We don't need any seeks for writes, where column values are primitives.
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(30_ColId)),
      PrimitiveValue("row1_c"), HybridTime::FromMicros(1000)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(40_ColId)),
      PrimitiveValue(10000), HybridTime::FromMicros(1000)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(50_ColId)),
      PrimitiveValue("row1_e"), HybridTime::FromMicros(1000)));

  // Row 2: one null column, one column that gets deleted and overwritten, another that just gets
  // overwritten. No seeks needed for writes.
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey2, PrimitiveValue(40_ColId)),
      PrimitiveValue(20000), HybridTime::FromMicros(2000)));

  // Deletions normally perform a lookup of the key to see whether it's already there. We will use
  // that to provide the expected result (the number of rows deleted in SQL or whether a key was
  // deleted in Redis). However, because we've just set a value at this path, we don't expect to
  // perform any reads for this deletion.
  ASSERT_OK(DeleteSubDoc(
      DocPath(kEncodedDocKey2, PrimitiveValue(40_ColId)),
      HybridTime::FromMicros(2500)));

  // The entire subdocument under DocPath(encoded_doc_key2, 40) just got deleted, and that fact
  // should still be in the write batch's cache, so we should not perform a seek to overwrite it.
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey2, PrimitiveValue(40_ColId)),
      PrimitiveValue(30000), HybridTime::FromMicros(3000)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey2, PrimitiveValue(50_ColId)),
      PrimitiveValue("row2_e"), HybridTime::FromMicros(2000)));

  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey2, PrimitiveValue(50_ColId)),
      PrimitiveValue("row2_e_prime"), HybridTime::FromMicros(4000)));

  ASSERT_DOCDB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(30); HT{ physical: 1000 }]) -> "row1_c"
      SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(40); HT{ physical: 1000 }]) -> 10000
      SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(50); HT{ physical: 1000 }]) -> "row1_e"
      SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(40); HT{ physical: 3000 }]) -> 30000
      SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(40); HT{ physical: 2500 }]) -> DEL
      SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(40); HT{ physical: 2000 }]) -> 20000
      SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(50); HT{ physical: 4000 }]) -> "row2_e_prime"
      SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(50); HT{ physical: 2000 }]) -> "row2_e"
      )#");

  const Schema &schema = kSchemaForIteratorTests;
  const Schema &projection = kProjectionForIteratorTests;
  QLTableRow row;
  QLValue value;

  {
    DocRowwiseIterator iter(
        projection, schema, kNonTransactionalOperationContext, doc_db(),
        CoarseTimePoint::max() /* deadline */, ReadHybridTime::FromMicros(2000));
    ASSERT_OK(iter.Init());

    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ("row1_c", value.string_value());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ(10000, value.int64_value());

    ASSERT_OK(row.GetValue(projection.column_id(2), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ("row1_e", value.string_value());

    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_TRUE(value.IsNull());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ(20000, value.int64_value());

    ASSERT_OK(row.GetValue(projection.column_id(2), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ("row2_e", value.string_value());

    ASSERT_FALSE(ASSERT_RESULT(iter.HasNext()));
  }

  // Scan at a later hybrid_time.

  {
    DocRowwiseIterator iter(
        projection, schema, kNonTransactionalOperationContext, doc_db(),
        CoarseTimePoint::max() /* deadline */, ReadHybridTime::FromMicros(5000));
    ASSERT_OK(iter.Init());

    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    // This row is exactly the same as in the previous case. TODO: deduplicate.

    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ("row1_c", value.string_value());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ(10000, value.int64_value());

    ASSERT_OK(row.GetValue(projection.column_id(2), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ("row1_e", value.string_value());

    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_TRUE(value.IsNull());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_FALSE(value.IsNull());

    // These two rows have different values compared to the previous case.
    ASSERT_EQ(30000, value.int64_value());

    ASSERT_OK(row.GetValue(projection.column_id(2), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ("row2_e_prime", value.string_value());

    ASSERT_FALSE(ASSERT_RESULT(iter.HasNext()));
  }
}

TEST_F(DocRowwiseIteratorTest, DocRowwiseIteratorDeletedDocumentTest) {
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(30_ColId)),
      PrimitiveValue("row1_c"), HybridTime::FromMicros(1000)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(40_ColId)),
      PrimitiveValue(10000), HybridTime::FromMicros(1000)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(50_ColId)),
      PrimitiveValue("row1_e"), HybridTime::FromMicros(1000)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey2, PrimitiveValue(40_ColId)),
      PrimitiveValue(20000), HybridTime::FromMicros(2000)));

  // Delete entire row1 document to test that iterator can successfully jump to next document
  // when it finds deleted document.
  ASSERT_OK(DeleteSubDoc(
      DocPath(kEncodedDocKey1), HybridTime::FromMicros(2500)));

  ASSERT_DOCDB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["row1", 11111]), [HT{ physical: 2500 }]) -> DEL
      SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(30); HT{ physical: 1000 }]) -> "row1_c"
      SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(40); HT{ physical: 1000 }]) -> 10000
      SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(50); HT{ physical: 1000 }]) -> "row1_e"
      SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(40); HT{ physical: 2000 }]) -> 20000
      )#");

  const Schema &schema = kSchemaForIteratorTests;
  const Schema &projection = kProjectionForIteratorTests;

  {
    DocRowwiseIterator iter(
        projection, schema, kNonTransactionalOperationContext, doc_db(),
        CoarseTimePoint::max() /* deadline */, ReadHybridTime::FromMicros(2500));
    ASSERT_OK(iter.Init());

    QLTableRow row;
    QLValue value;

    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_TRUE(value.IsNull());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ(20000, value.int64_value());

    ASSERT_OK(row.GetValue(projection.column_id(2), &value));
    ASSERT_TRUE(value.IsNull());

    ASSERT_FALSE(ASSERT_RESULT(iter.HasNext()));
  }
}

TEST_F(DocRowwiseIteratorTest, DocRowwiseIteratorTestRowDeletes) {
  auto dwb = MakeDocWriteBatch();

  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey1, PrimitiveValue(30_ColId)),
      PrimitiveValue("row1_c")));

  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey1, PrimitiveValue(40_ColId)),
      PrimitiveValue(10000)));
  ASSERT_OK(WriteToRocksDBAndClear(&dwb, HybridTime::FromMicros(1000)));

  ASSERT_OK(dwb.DeleteSubDoc(DocPath(kEncodedDocKey1)));
  ASSERT_OK(WriteToRocksDBAndClear(&dwb, HybridTime::FromMicros(2500)));

  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey1, PrimitiveValue(50_ColId)),
      PrimitiveValue("row1_e")));

  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey2, PrimitiveValue(40_ColId)),
      PrimitiveValue(20000)));
  ASSERT_OK(WriteToRocksDB(dwb, HybridTime::FromMicros(2800)));

  ASSERT_DOCDB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["row1", 11111]), [HT{ physical: 2500 }]) -> DEL
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(30); HT{ physical: 1000 }]) -> "row1_c"
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(40); HT{ physical: 1000 w: 1 }]) -> 10000
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(50); HT{ physical: 2800 }]) -> "row1_e"
SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(40); HT{ physical: 2800 w: 1 }]) -> 20000
      )#");

  const Schema &schema = kSchemaForIteratorTests;
  const Schema &projection = kProjectionForIteratorTests;

  {
    DocRowwiseIterator iter(
        projection, schema, kNonTransactionalOperationContext, doc_db(),
        CoarseTimePoint::max() /* deadline */, ReadHybridTime::FromMicros(2800));
    ASSERT_OK(iter.Init());

    QLTableRow row;
    QLValue value;

    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    // ColumnId 30, 40 should be hidden whereas ColumnId 50 should be visible.
    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_TRUE(value.IsNull());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_TRUE(value.IsNull());

    ASSERT_OK(row.GetValue(projection.column_id(2), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ("row1_e", value.string_value());

    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_TRUE(value.IsNull());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ(20000, value.int64_value());

    ASSERT_OK(row.GetValue(projection.column_id(2), &value));
    ASSERT_TRUE(value.IsNull());
  }
}

void VerifyOldestRecordTime(IntentAwareIterator *iter, const DocKey &doc_key,
                            const SubDocKey &subkey, HybridTime min_hybrid_time,
                            HybridTime expected_oldest_record_time) {
  iter->Seek(doc_key);
  const KeyBytes subkey_bytes = subkey.EncodeWithoutHt();
  const Slice subkey_slice = subkey_bytes.AsSlice();
  Slice read_value;
  HybridTime oldest_past_min_ht =
      ASSERT_RESULT(iter->FindOldestRecord(subkey_slice, min_hybrid_time));
  LOG(INFO) << "iter->FindOldestRecord returned " << oldest_past_min_ht
            << " for " << SubDocKey::DebugSliceToString(subkey_slice);
  ASSERT_EQ(oldest_past_min_ht, expected_oldest_record_time);
}

void VerifyOldestRecordTime(IntentAwareIterator *iter, const DocKey &doc_key,
                            const SubDocKey &subkey, uint64_t min_hybrid_time,
                            uint64_t expected_oldest_record_time) {
  VerifyOldestRecordTime(iter, doc_key, subkey,
                         HybridTime::FromMicros(min_hybrid_time),
                         HybridTime::FromMicros(expected_oldest_record_time));
}

void VerifyOldestRecordTimeIsInvalid(IntentAwareIterator *iter,
                                     const DocKey &doc_key,
                                     const SubDocKey &subkey,
                                     uint64_t min_hybrid_time) {
  VerifyOldestRecordTime(iter, doc_key, subkey,
                         HybridTime::FromMicros(min_hybrid_time),
                         HybridTime::kInvalid);
}

TEST_F(DocRowwiseIteratorTest, BackfillInsert) {
  ASSERT_OK(DeleteSubDoc(DocPath(kEncodedDocKey1), 5000_usec_ht));
  ASSERT_OK(SetPrimitive(DocPath(kEncodedDocKey1, PrimitiveValue(40_ColId)),
                         PrimitiveValue(10000), 1000_usec_ht));

  ASSERT_OK(SetPrimitive(DocPath(kEncodedDocKey1, PrimitiveValue(50_ColId)),
                         PrimitiveValue("row1_e"), 1000_usec_ht));

  ASSERT_OK(SetPrimitive(DocPath(kEncodedDocKey1, PrimitiveValue(40_ColId)),
                         PrimitiveValue(10000), 900_usec_ht));

  ASSERT_OK(SetPrimitive(DocPath(kEncodedDocKey1, PrimitiveValue(50_ColId)),
                         PrimitiveValue("row1_e"), 900_usec_ht));

  ASSERT_OK(DeleteSubDoc(DocPath(kEncodedDocKey1), 500_usec_ht));
  ASSERT_OK(SetPrimitive(DocPath(kEncodedDocKey1, PrimitiveValue(40_ColId)),
                         PrimitiveValue(10000), 300_usec_ht));

  ASSERT_OK(SetPrimitive(DocPath(kEncodedDocKey1, PrimitiveValue(50_ColId)),
                         PrimitiveValue("row1_e"), 300_usec_ht));

  ASSERT_OK(DeleteSubDoc(DocPath(kEncodedDocKey2), 900_usec_ht));
  ASSERT_OK(DeleteSubDoc(DocPath(kEncodedDocKey2), 700_usec_ht));

  SetTransactionIsolationLevel(IsolationLevel::SNAPSHOT_ISOLATION);
  Result<TransactionId> txn1 = FullyDecodeTransactionId("0000000000000001");
  ASSERT_OK(txn1);
  SetCurrentTransactionId(*txn1);
  ASSERT_OK(DeleteSubDoc(DocPath(kEncodedDocKey2), 800_usec_ht));

  ASSERT_DOCDB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["row1", 11111]), [HT{ physical: 5000 }]) -> DEL
SubDocKey(DocKey([], ["row1", 11111]), [HT{ physical: 500 }]) -> DEL
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(40); HT{ physical: 1000 }]) -> 10000
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(40); HT{ physical: 900 }]) -> 10000
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(40); HT{ physical: 300 }]) -> 10000
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(50); HT{ physical: 1000 }]) -> "row1_e"
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(50); HT{ physical: 900 }]) -> "row1_e"
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(50); HT{ physical: 300 }]) -> "row1_e"
SubDocKey(DocKey([], ["row2", 22222]), [HT{ physical: 900 }]) -> DEL
SubDocKey(DocKey([], ["row2", 22222]), [HT{ physical: 700 }]) -> DEL
SubDocKey(DocKey([], []), []) [kWeakRead, kWeakWrite] HT{ physical: 800 w: 1 } -> \
  TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["row2"]), []) [kWeakRead, kWeakWrite] HT{ physical: 800 w: 2 } -> \
  TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["row2", 22222]), []) [kStrongRead, kStrongWrite] HT{ physical: 800 } -> \
  TransactionId(30303030-3030-3030-3030-303030303031) WriteId(0) DEL
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 800 } -> \
  SubDocKey(DocKey([], ["row2", 22222]), []) [kStrongRead, kStrongWrite] HT{ physical: 800 }
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 800 w: 1 } -> \
  SubDocKey(DocKey([], []), []) [kWeakRead, kWeakWrite] HT{ physical: 800 w: 1 }
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 800 w: 2 } -> \
  SubDocKey(DocKey([], ["row2"]), []) [kWeakRead, kWeakWrite] HT{ physical: 800 w: 2 }
      )#");

  TransactionStatusManagerMock myTransactionalOperationContext;
  const TransactionOperationContext kMockTransactionalOperationContext = {
      TransactionId::GenerateRandom(), &myTransactionalOperationContext};
  myTransactionalOperationContext.Commit(*txn1, 800_usec_ht);

  const HybridTime kSafeTime = 50000_usec_ht;
  {
    DocKey doc_key(PrimitiveValues("row1", 11111));
    const KeyBytes doc_key_bytes = doc_key.Encode();
    boost::optional<const yb::Slice> doc_key_optional(doc_key_bytes.AsSlice());
    auto iter = CreateIntentAwareIterator(
        doc_db(), BloomFilterMode::USE_BLOOM_FILTER, doc_key_optional,
        rocksdb::kDefaultQueryId, kMockTransactionalOperationContext,
        CoarseTimePoint::max(), ReadHybridTime::SingleTime(kSafeTime));

    {
      SubDocKey subkey(doc_key);
      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 499, 500);
      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 500, 5000);
      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 501, 5000);

      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 4999, 5000);
      VerifyOldestRecordTimeIsInvalid(iter.get(), doc_key, subkey, 5000);
      VerifyOldestRecordTimeIsInvalid(iter.get(), doc_key, subkey, 5001);
    }

    {
      SubDocKey subkey(doc_key, PrimitiveValue(40_ColId));
      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 299, 300);
      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 300, 900);
      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 301, 900);

      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 500, 900);
      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 600, 900);

      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 899, 900);
      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 900, 1000);
      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 901, 1000);

      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 999, 1000);
      VerifyOldestRecordTimeIsInvalid(iter.get(), doc_key, subkey, 1000);
      VerifyOldestRecordTimeIsInvalid(iter.get(), doc_key, subkey, 1001);
      VerifyOldestRecordTimeIsInvalid(iter.get(), doc_key, subkey, 40000);
    }
  }

  {
    DocKey doc_key(PrimitiveValues("row2", 22222));
    const KeyBytes doc_key_bytes = doc_key.Encode();
    boost::optional<const yb::Slice> doc_key_optional(doc_key_bytes.AsSlice());
    auto iter = CreateIntentAwareIterator(
        doc_db(), BloomFilterMode::USE_BLOOM_FILTER, doc_key_optional,
        rocksdb::kDefaultQueryId, kMockTransactionalOperationContext,
        CoarseTimePoint::max(), ReadHybridTime::SingleTime(kSafeTime));

    {
      SubDocKey subkey(doc_key);
      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 400, 700);
      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 699, 700);
      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 700, 800);
      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 701, 800);

      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 750, 800);
      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 800, 900);
      VerifyOldestRecordTime(iter.get(), doc_key, subkey, 801, 900);
      VerifyOldestRecordTimeIsInvalid(iter.get(), doc_key, subkey, 900);
      VerifyOldestRecordTimeIsInvalid(iter.get(), doc_key, subkey, 1000);
    }
  }
}

TEST_F(DocRowwiseIteratorTest, DocRowwiseIteratorHasNextIdempotence) {
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(40_ColId)),
      PrimitiveValue(10000), HybridTime::FromMicros(1000)));

  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(50_ColId)),
      PrimitiveValue("row1_e"), HybridTime::FromMicros(2800)));

  ASSERT_OK(DeleteSubDoc(DocPath(kEncodedDocKey1), HybridTime::FromMicros(2500)));

  ASSERT_DOCDB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["row1", 11111]), [HT{ physical: 2500 }]) -> DEL
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(40); HT{ physical: 1000 }]) -> 10000
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(50); HT{ physical: 2800 }]) -> "row1_e"
      )#");

  const Schema &schema = kSchemaForIteratorTests;
  const Schema &projection = kProjectionForIteratorTests;

  {
    DocRowwiseIterator iter(
        projection, schema, kNonTransactionalOperationContext, doc_db(),
        CoarseTimePoint::max() /* deadline */, ReadHybridTime::FromMicros(2800));
    ASSERT_OK(iter.Init());

    QLTableRow row;
    QLValue value;

    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    // Ensure calling HasNext() again doesn't mess up anything.
    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    // ColumnId 40 should be deleted whereas ColumnId 50 should be visible.
    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_TRUE(value.IsNull());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_TRUE(value.IsNull());

    ASSERT_OK(row.GetValue(projection.column_id(2), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ("row1_e", value.string_value());
  }
}

TEST_F(DocRowwiseIteratorTest, DocRowwiseIteratorIncompleteProjection) {
  auto dwb = MakeDocWriteBatch();

  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey1, PrimitiveValue(40_ColId)),
      PrimitiveValue(10000)));
  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey1, PrimitiveValue(50_ColId)),
      PrimitiveValue("row1_e")));
  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey2, PrimitiveValue(40_ColId)),
      PrimitiveValue(20000)));

  ASSERT_OK(WriteToRocksDB(dwb, HybridTime::FromMicros(1000)));

  ASSERT_DOCDB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(40); HT{ physical: 1000 }]) -> 10000
      SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(50); HT{ physical: 1000 w: 1 }]) -> "row1_e"
      SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(40); HT{ physical: 1000 w: 2 }]) -> 20000
      )#");

  const Schema &schema = kSchemaForIteratorTests;
  Schema projection;
  ASSERT_OK(kSchemaForIteratorTests.CreateProjectionByNames({"c", "d"},
      &projection));

  {
    DocRowwiseIterator iter(
        projection, schema, kNonTransactionalOperationContext, doc_db(),
        CoarseTimePoint::max() /* deadline */, ReadHybridTime::FromMicros(2800));
    ASSERT_OK(iter.Init());

    QLTableRow row;
    QLValue value;

    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_TRUE(value.IsNull());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ(10000, value.int64_value());

    // Now find next row.
    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_TRUE(value.IsNull());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ(20000, value.int64_value());

    ASSERT_FALSE(ASSERT_RESULT(iter.HasNext()));
  }
}

TEST_F(DocRowwiseIteratorTest, DocRowwiseIteratorMultipleDeletes) {
  auto dwb = MakeDocWriteBatch();

  MonoDelta ttl = MonoDelta::FromMilliseconds(1);
  MonoDelta ttl_expiry = MonoDelta::FromMilliseconds(2);
  auto read_time = ReadHybridTime::SingleTime(server::HybridClock::AddPhysicalTimeToHybridTime(
      HybridTime::FromMicros(2800), ttl_expiry));

  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey1, PrimitiveValue(30_ColId)),
      PrimitiveValue("row1_c")));
  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey1, PrimitiveValue(40_ColId)),
      PrimitiveValue(10000)));
  ASSERT_OK(WriteToRocksDBAndClear(&dwb, HybridTime::FromMicros(1000)));

  // Deletes.
  ASSERT_OK(dwb.DeleteSubDoc(DocPath(kEncodedDocKey1)));
  ASSERT_OK(dwb.DeleteSubDoc(DocPath(kEncodedDocKey2)));
  ASSERT_OK(WriteToRocksDBAndClear(&dwb, HybridTime::FromMicros(2500)));
  dwb.Clear();

  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey1, PrimitiveValue(50_ColId)),
      Value(PrimitiveValue("row1_e"), ttl)));

  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey2, PrimitiveValue(30_ColId)),
      PrimitiveValue::kTombstone));
  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey2, PrimitiveValue(40_ColId)),
      PrimitiveValue(20000)));
  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey2, PrimitiveValue(50_ColId)),
      Value(PrimitiveValue("row2_e"), MonoDelta::FromMilliseconds(3))));
  ASSERT_OK(WriteToRocksDBAndClear(&dwb, HybridTime::FromMicros(2800)));

  ASSERT_OK(WriteToRocksDB(dwb, HybridTime::FromMicros(1000)));

  ASSERT_DOCDB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["row1", 11111]), [HT{ physical: 2500 }]) -> DEL
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(30); HT{ physical: 1000 }]) -> "row1_c"
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(40); HT{ physical: 1000 w: 1 }]) -> 10000
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(50); HT{ physical: 2800 }]) -> \
    "row1_e"; ttl: 0.001s
SubDocKey(DocKey([], ["row2", 22222]), [HT{ physical: 2500 w: 1 }]) -> DEL
SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(30); HT{ physical: 2800 w: 1 }]) -> DEL
SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(40); HT{ physical: 2800 w: 2 }]) -> 20000
SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(50); HT{ physical: 2800 w: 3 }]) -> \
    "row2_e"; ttl: 0.003s
      )#");

  const Schema &schema = kSchemaForIteratorTests;
  Schema projection;
  ASSERT_OK(kSchemaForIteratorTests.CreateProjectionByNames({"c", "e"}, &projection));

  {
    DocRowwiseIterator iter(
        projection, schema, kNonTransactionalOperationContext, doc_db(),
        CoarseTimePoint::max() /* deadline */, read_time);
    ASSERT_OK(iter.Init());

    QLTableRow row;
    QLValue value;

    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    // Ensure Idempotency.
    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_TRUE(value.IsNull());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ("row2_e", value.string_value());

    ASSERT_FALSE(ASSERT_RESULT(iter.HasNext()));
  }
}

TEST_F(DocRowwiseIteratorTest, DocRowwiseIteratorValidColumnNotInProjection) {
  auto dwb = MakeDocWriteBatch();

  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey1, PrimitiveValue(40_ColId)),
      PrimitiveValue(10000)));
  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey2, PrimitiveValue(40_ColId)),
      PrimitiveValue(20000)));
  ASSERT_OK(WriteToRocksDBAndClear(&dwb, HybridTime::FromMicros(1000)));

  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey2, PrimitiveValue(50_ColId)),
      PrimitiveValue("row2_e")));
  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey2, PrimitiveValue(30_ColId)),
      PrimitiveValue("row2_c")));
  ASSERT_OK(WriteToRocksDBAndClear(&dwb, HybridTime::FromMicros(2000)));

  ASSERT_OK(dwb.DeleteSubDoc(DocPath(kEncodedDocKey1)));
  ASSERT_OK(WriteToRocksDBAndClear(&dwb, HybridTime::FromMicros(2500)));

  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey1, PrimitiveValue(50_ColId)),
      PrimitiveValue("row1_e")));
  ASSERT_OK(WriteToRocksDBAndClear(&dwb, HybridTime::FromMicros(2800)));


  ASSERT_DOCDB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["row1", 11111]), [HT{ physical: 2500 }]) -> DEL
      SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(40); HT{ physical: 1000 }]) -> 10000
      SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(50); HT{ physical: 2800 }]) -> "row1_e"
      SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(30); HT{ physical: 2000 w: 1 }]) -> "row2_c"
      SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(40); HT{ physical: 1000 w: 1 }]) -> 20000
      SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(50); HT{ physical: 2000 }]) -> "row2_e"
      )#");

  const Schema &schema = kSchemaForIteratorTests;
  Schema projection;
  ASSERT_OK(kSchemaForIteratorTests.CreateProjectionByNames({"c", "d"}, &projection));

  {
    DocRowwiseIterator iter(
        projection, schema, kNonTransactionalOperationContext, doc_db(),
        CoarseTimePoint::max() /* deadline */, ReadHybridTime::FromMicros(2800));
    ASSERT_OK(iter.Init());

    QLTableRow row;
    QLValue value;

    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_TRUE(value.IsNull());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_TRUE(value.IsNull());

    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ("row2_c", value.string_value());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ(20000, value.int64_value());

    ASSERT_FALSE(ASSERT_RESULT(iter.HasNext()));
  }
}

TEST_F(DocRowwiseIteratorTest, DocRowwiseIteratorKeyProjection) {
  auto dwb = MakeDocWriteBatch();

  // Row 1
  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey1, PrimitiveValue(40_ColId)),
      PrimitiveValue(10000)));
  ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey1, PrimitiveValue(50_ColId)),
      PrimitiveValue("row1_e")));

  ASSERT_OK(WriteToRocksDB(dwb, HybridTime::FromMicros(1000)));

  ASSERT_DOCDB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(40); HT{ physical: 1000 }]) -> 10000
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(50); HT{ physical: 1000 w: 1 }]) -> "row1_e"
      )#");

  const Schema &schema = kSchemaForIteratorTests;
  Schema projection;
  ASSERT_OK(kSchemaForIteratorTests.CreateProjectionByNames({"a", "b"},
      &projection, 2));

  {
    DocRowwiseIterator iter(
        projection, schema, kNonTransactionalOperationContext, doc_db(),
        CoarseTimePoint::max() /* deadline */, ReadHybridTime::FromMicros(2800));
    ASSERT_OK(iter.Init());

    QLTableRow row;
    QLValue value;

    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_EQ("row1", value.string_value());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_EQ(11111, value.int64_value());

    ASSERT_FALSE(ASSERT_RESULT(iter.HasNext()));
  }
}

TEST_F(DocRowwiseIteratorTest, DocRowwiseIteratorResolveWriteIntents) {
  SetTransactionIsolationLevel(IsolationLevel::SNAPSHOT_ISOLATION);

  TransactionStatusManagerMock txn_status_manager;

  auto txn1 = ASSERT_RESULT(FullyDecodeTransactionId("0000000000000001"));
  auto txn2 = ASSERT_RESULT(FullyDecodeTransactionId("0000000000000002"));

  SetCurrentTransactionId(txn1);
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(30_ColId)),
      PrimitiveValue("row1_c_t1"), HybridTime::FromMicros(500)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(40_ColId)),
      PrimitiveValue(40000), HybridTime::FromMicros(500)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(50_ColId)),
      PrimitiveValue("row1_e_t1"), HybridTime::FromMicros(500)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey2, PrimitiveValue(40_ColId)),
      PrimitiveValue(42000), HybridTime::FromMicros(500)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey2, PrimitiveValue(50_ColId)),
      PrimitiveValue("row2_e_t1"), HybridTime::FromMicros(500)));
  ResetCurrentTransactionId();

  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(30_ColId)),
      PrimitiveValue("row1_c"), HybridTime::FromMicros(1000)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(40_ColId)),
      PrimitiveValue(10000), HybridTime::FromMicros(1000)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(50_ColId)),
      PrimitiveValue("row1_e"), HybridTime::FromMicros(1000)));

  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey2, PrimitiveValue(40_ColId)),
      PrimitiveValue(20000), HybridTime::FromMicros(2000)));

  ASSERT_OK(DeleteSubDoc(
      DocPath(kEncodedDocKey2, PrimitiveValue(40_ColId)),
      HybridTime::FromMicros(2500)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey2, PrimitiveValue(40_ColId)),
      PrimitiveValue(30000), HybridTime::FromMicros(3000)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey2, PrimitiveValue(50_ColId)),
      PrimitiveValue("row2_e"), HybridTime::FromMicros(2000)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey2, PrimitiveValue(50_ColId)),
      PrimitiveValue("row2_e_prime"), HybridTime::FromMicros(4000)));

  txn_status_manager.Commit(txn1, HybridTime::FromMicros(3500));

  SetCurrentTransactionId(txn2);
  ASSERT_OK(DeleteSubDoc(
      DocPath(kEncodedDocKey1),
      HybridTime::FromMicros(4000)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey2, PrimitiveValue(50_ColId)),
      PrimitiveValue("row2_e_t2"), HybridTime::FromMicros(4000)));
  ResetCurrentTransactionId();
  txn_status_manager.Commit(txn2, HybridTime::FromMicros(6000));

  ASSERT_DOCDB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(30); HT{ physical: 1000 }]) -> "row1_c"
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(40); HT{ physical: 1000 }]) -> 10000
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(50); HT{ physical: 1000 }]) -> "row1_e"
SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(40); HT{ physical: 3000 }]) -> 30000
SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(40); HT{ physical: 2500 }]) -> DEL
SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(40); HT{ physical: 2000 }]) -> 20000
SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(50); HT{ physical: 4000 }]) -> "row2_e_prime"
SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(50); HT{ physical: 2000 }]) -> "row2_e"
SubDocKey(DocKey([], []), []) [kWeakRead, kWeakWrite] HT{ physical: 4000 w: 1 } -> \
    TransactionId(30303030-3030-3030-3030-303030303032) none
SubDocKey(DocKey([], []), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 1 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["row1"]), []) [kWeakRead, kWeakWrite] HT{ physical: 4000 w: 2 } -> \
    TransactionId(30303030-3030-3030-3030-303030303032) none
SubDocKey(DocKey([], ["row1"]), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 2 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["row1", 11111]), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 3 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["row1", 11111]), []) [kStrongRead, kStrongWrite] HT{ physical: 4000 } -> \
    TransactionId(30303030-3030-3030-3030-303030303032) WriteId(5) DEL
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(30)]) [kStrongRead, kStrongWrite] \
    HT{ physical: 500 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) WriteId(0) "row1_c_t1"
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(40)]) [kStrongRead, kStrongWrite] \
    HT{ physical: 500 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) WriteId(1) 40000
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(50)]) [kStrongRead, kStrongWrite] \
    HT{ physical: 500 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) WriteId(2) "row1_e_t1"
SubDocKey(DocKey([], ["row2"]), []) [kWeakRead, kWeakWrite] HT{ physical: 4000 w: 2 } -> \
    TransactionId(30303030-3030-3030-3030-303030303032) none
SubDocKey(DocKey([], ["row2"]), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 2 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["row2", 22222]), []) [kWeakRead, kWeakWrite] HT{ physical: 4000 w: 3 } -> \
    TransactionId(30303030-3030-3030-3030-303030303032) none
SubDocKey(DocKey([], ["row2", 22222]), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 3 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(40)]) [kStrongRead, kStrongWrite] \
    HT{ physical: 500 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) WriteId(3) 42000
SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(50)]) [kStrongRead, kStrongWrite] \
    HT{ physical: 4000 } \
    -> TransactionId(30303030-3030-3030-3030-303030303032) WriteId(6) "row2_e_t2"
SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(50)]) [kStrongRead, kStrongWrite] \
    HT{ physical: 500 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) WriteId(4) "row2_e_t1"
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 500 } -> \
    SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(50)]) [kStrongRead, kStrongWrite] \
    HT{ physical: 500 }
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 500 w: 1 } -> \
    SubDocKey(DocKey([], []), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 1 }
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 500 w: 2 } -> \
    SubDocKey(DocKey([], ["row2"]), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 2 }
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 500 w: 3 } -> \
    SubDocKey(DocKey([], ["row2", 22222]), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 3 }
TXN REV 30303030-3030-3030-3030-303030303032 HT{ physical: 4000 } -> \
    SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(50)]) [kStrongRead, kStrongWrite] \
    HT{ physical: 4000 }
TXN REV 30303030-3030-3030-3030-303030303032 HT{ physical: 4000 w: 1 } -> \
    SubDocKey(DocKey([], []), []) [kWeakRead, kWeakWrite] HT{ physical: 4000 w: 1 }
TXN REV 30303030-3030-3030-3030-303030303032 HT{ physical: 4000 w: 2 } -> \
    SubDocKey(DocKey([], ["row2"]), []) [kWeakRead, kWeakWrite] HT{ physical: 4000 w: 2 }
TXN REV 30303030-3030-3030-3030-303030303032 HT{ physical: 4000 w: 3 } -> \
    SubDocKey(DocKey([], ["row2", 22222]), []) [kWeakRead, kWeakWrite] HT{ physical: 4000 w: 3 }
      )#");

  const Schema &schema = kSchemaForIteratorTests;
  const Schema &projection = kProjectionForIteratorTests;
  const auto txn_context = TransactionOperationContext(
      TransactionId::GenerateRandom(), &txn_status_manager);

  {
    DocRowwiseIterator iter(
        projection, schema, txn_context, doc_db(),
        CoarseTimePoint::max() /* deadline */, ReadHybridTime::FromMicros(2000));
    ASSERT_OK(iter.Init());

    QLTableRow row;
    QLValue value;

    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ("row1_c", value.string_value());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ(10000, value.int64_value());

    ASSERT_OK(row.GetValue(projection.column_id(2), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ("row1_e", value.string_value());

    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_TRUE(value.IsNull());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ(20000, value.int64_value());

    ASSERT_OK(row.GetValue(projection.column_id(2), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ("row2_e", value.string_value());

    ASSERT_FALSE(ASSERT_RESULT(iter.HasNext()));
  }

  // Scan at a later hybrid_time.

  LOG(INFO) << "===============================================";
  {
    DocRowwiseIterator iter(
        projection, schema, txn_context, doc_db(),
        CoarseTimePoint::max() /* deadline */, ReadHybridTime::FromMicros(5000));
    ASSERT_OK(iter.Init());
    QLTableRow row;
    QLValue value;

    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ("row1_c_t1", value.string_value());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ(40000, value.int64_value());

    ASSERT_OK(row.GetValue(projection.column_id(2), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ("row1_e_t1", value.string_value());

    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_TRUE(value.IsNull());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ(42000, value.int64_value());

    ASSERT_OK(row.GetValue(projection.column_id(2), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ("row2_e_prime", value.string_value());

    ASSERT_FALSE(ASSERT_RESULT(iter.HasNext()));
  }

  // Scan at a later hybrid_time.

  {
    DocRowwiseIterator iter(
        projection, schema, txn_context, doc_db(),
        CoarseTimePoint::max() /* deadline */, ReadHybridTime::FromMicros(6000));
    ASSERT_OK(iter.Init());

    QLTableRow row;
    QLValue value;

    ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
    ASSERT_OK(iter.NextRow(&row));

    ASSERT_OK(row.GetValue(projection.column_id(0), &value));
    ASSERT_TRUE(value.IsNull());

    ASSERT_OK(row.GetValue(projection.column_id(1), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ(42000, value.int64_value());

    ASSERT_OK(row.GetValue(projection.column_id(2), &value));
    ASSERT_FALSE(value.IsNull());
    ASSERT_EQ("row2_e_t2", value.string_value());

    ASSERT_FALSE(ASSERT_RESULT(iter.HasNext()));
  }
}

TEST_F(DocRowwiseIteratorTest, IntentAwareIteratorSeek) {
  SetTransactionIsolationLevel(IsolationLevel::SNAPSHOT_ISOLATION);

  TransactionStatusManagerMock txn_status_manager;

  Result<TransactionId> txn = FullyDecodeTransactionId("0000000000000001");
  ASSERT_OK(txn);

  // Have a mix of transactional / non-transaction writes.
  SetCurrentTransactionId(*txn);
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(30_ColId)),
      PrimitiveValue("row1_c_txn"), HybridTime::FromMicros(500)));

  txn_status_manager.Commit(*txn, HybridTime::FromMicros(600));

  ResetCurrentTransactionId();

  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(30_ColId)),
      PrimitiveValue("row1_c"), HybridTime::FromMicros(1000)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(40_ColId)),
      PrimitiveValue(10000), HybridTime::FromMicros(1000)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey2, PrimitiveValue(30_ColId)),
      PrimitiveValue("row2_c"), HybridTime::FromMicros(1000)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey2, PrimitiveValue(40_ColId)),
      PrimitiveValue(20000), HybridTime::FromMicros(1000)));

  // Verify the content of RocksDB.
  ASSERT_DOCDB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(30); HT{ physical: 1000 }]) -> "row1_c"
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(40); HT{ physical: 1000 }]) -> 10000
SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(30); HT{ physical: 1000 }]) -> "row2_c"
SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(40); HT{ physical: 1000 }]) -> 20000
SubDocKey(DocKey([], []), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 1 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["row1"]), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 2 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["row1", 11111]), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 3 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(30)]) [kStrongRead, kStrongWrite] \
    HT{ physical: 500 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) WriteId(0) "row1_c_txn"
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 500 } -> \
    SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(30)]) [kStrongRead, kStrongWrite] \
    HT{ physical: 500 }
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 500 w: 1 } -> \
    SubDocKey(DocKey([], []), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 1 }
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 500 w: 2 } -> \
    SubDocKey(DocKey([], ["row1"]), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 2 }
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 500 w: 3 } -> \
    SubDocKey(DocKey([], ["row1", 11111]), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 3 }
    )#");

  // Create a new IntentAwareIterator and seek to an empty DocKey. Verify that it returns the
  // first non-intent key.
  IntentAwareIterator iter(
      doc_db(), rocksdb::ReadOptions(), CoarseTimePoint::max() /* deadline */,
      ReadHybridTime::FromMicros(1000), boost::none);
  iter.Seek(DocKey());
  ASSERT_TRUE(iter.valid());
  auto key_data = ASSERT_RESULT(iter.FetchKey());
  SubDocKey subdoc_key;
  ASSERT_OK(subdoc_key.FullyDecodeFrom(key_data.key, HybridTimeRequired::kFalse));
  ASSERT_EQ(subdoc_key.ToString(), R"#(SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(30)]))#");
  ASSERT_EQ(key_data.write_time.ToString(), "HT{ physical: 1000 }");
}

TEST_F(DocRowwiseIteratorTest, SeekTwiceWithinTheSameTxn) {
  SetTransactionIsolationLevel(IsolationLevel::SNAPSHOT_ISOLATION);

  TransactionStatusManagerMock txn_status_manager;

  Result<TransactionId> txn = FullyDecodeTransactionId("0000000000000001");
  ASSERT_OK(txn);

  SetCurrentTransactionId(*txn);
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(30_ColId)),
      PrimitiveValue("row1_c_t1"), HybridTime::FromMicros(500)));

  // Verify the content of RocksDB.
  ASSERT_DOCDB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], []), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 1 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["row1"]), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 2 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["row1", 11111]), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 3 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(30)]) [kStrongRead, kStrongWrite] \
    HT{ physical: 500 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) WriteId(0) "row1_c_t1"
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 500 } -> \
    SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(30)]) [kStrongRead, kStrongWrite] \
    HT{ physical: 500 }
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 500 w: 1 } -> \
    SubDocKey(DocKey([], []), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 1 }
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 500 w: 2 } -> \
    SubDocKey(DocKey([], ["row1"]), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 2 }
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 500 w: 3 } -> \
    SubDocKey(DocKey([], ["row1", 11111]), []) [kWeakRead, kWeakWrite] HT{ physical: 500 w: 3 }
      )#");

  IntentAwareIterator iter(
      doc_db(), rocksdb::ReadOptions(), CoarseTimePoint::max() /* deadline */,
      ReadHybridTime::FromMicros(1000), TransactionOperationContext(*txn, &txn_status_manager));
  for (int i = 1; i <= 2; ++i) {
    iter.Seek(DocKey());
    ASSERT_TRUE(iter.valid()) << "Seek #" << i << " failed";
  }
}

TEST_F(DocRowwiseIteratorTest, ScanWithinTheSameTxn) {
  SetTransactionIsolationLevel(IsolationLevel::SNAPSHOT_ISOLATION);

  TransactionStatusManagerMock txn_status_manager;

  Result<TransactionId> txn = FullyDecodeTransactionId("0000000000000001");
  ASSERT_OK(txn);

  SetCurrentTransactionId(*txn);
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey2, PrimitiveValue(30_ColId)),
      PrimitiveValue("row2_c_t1"), HybridTime::FromMicros(500)));
  ASSERT_OK(SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(30_ColId)),
      PrimitiveValue("row1_c_t1"), HybridTime::FromMicros(600)));

  LOG(INFO) << "Dump:\n" << DocDBDebugDumpToStr();

  const auto txn_context = TransactionOperationContext(*txn, &txn_status_manager);
  const Schema &projection = kProjectionForIteratorTests;

  DocRowwiseIterator iter(
      projection, kSchemaForIteratorTests, txn_context, doc_db(),
      CoarseTimePoint::max() /* deadline */, ReadHybridTime::FromMicros(1000));
  ASSERT_OK(iter.Init());

  QLTableRow row;
  QLValue value;

  ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
  ASSERT_OK(iter.NextRow(&row));

  ASSERT_OK(row.GetValue(projection.column_id(0), &value));
  ASSERT_FALSE(value.IsNull());
  ASSERT_EQ("row1_c_t1", value.string_value());

  ASSERT_OK(row.GetValue(projection.column_id(1), &value));
  ASSERT_TRUE(value.IsNull());

  ASSERT_OK(row.GetValue(projection.column_id(2), &value));
  ASSERT_TRUE(value.IsNull());

  ASSERT_TRUE(ASSERT_RESULT(iter.HasNext()));
  ASSERT_OK(iter.NextRow(&row));

  ASSERT_OK(row.GetValue(projection.column_id(0), &value));
  ASSERT_FALSE(value.IsNull());
  ASSERT_EQ("row2_c_t1", value.string_value());

  ASSERT_OK(row.GetValue(projection.column_id(1), &value));
  ASSERT_TRUE(value.IsNull());

  ASSERT_OK(row.GetValue(projection.column_id(2), &value));
  ASSERT_TRUE(value.IsNull());

  ASSERT_FALSE(ASSERT_RESULT(iter.HasNext()));

  // Empirically we require 6 seeks to perform this test.
  // If this number increased, then something got broken and should be fixed.
  // IF this number decreased because of optimization, then we should adjust this check.
  ASSERT_EQ(intents_db_options_.statistics->getTickerCount(rocksdb::Tickers::NUMBER_DB_SEEK), 6);
}

}  // namespace docdb
}  // namespace yb
