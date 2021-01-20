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

#include "yb/docdb/docdb.h"

#include <memory>
#include <string>

#include "yb/common/doc_hybrid_time.h"
#include "yb/common/ql_value.h"
#include "yb/docdb/doc_key.h"
#include "yb/docdb/primitive_value.h"
#include "yb/rocksdb/db.h"
#include "yb/rocksdb/db/db_impl.h"
#include "yb/rocksdb/db/version_set.h"
#include "yb/rocksdb/db/writebuffer.h"
#include "yb/rocksdb/util/statistics.h"

#include "yb/common/hybrid_time.h"
#include "yb/docdb/doc_reader.h"
#include "yb/docdb/doc_reader_redis.h"
#include "yb/docdb/docdb_debug.h"
#include "yb/docdb/docdb-internal.h"
#include "yb/docdb/docdb_compaction_filter.h"
#include "yb/docdb/docdb_test_base.h"
#include "yb/docdb/docdb_test_util.h"
#include "yb/docdb/in_mem_docdb.h"
#include "yb/docdb/intent.h"
#include "yb/gutil/stringprintf.h"
#include "yb/gutil/walltime.h"
#include "yb/rocksutil/yb_rocksdb.h"
#include "yb/tablet/tablet_options.h"
#include "yb/server/hybrid_clock.h"
#include "yb/docdb/consensus_frontier.h"
#include "yb/docdb/docdb_rocksdb_util.h"

#include "yb/util/minmax.h"
#include "yb/util/net/net_util.h"
#include "yb/util/path_util.h"
#include "yb/util/random_util.h"
#include "yb/util/size_literals.h"
#include "yb/util/string_trim.h"
#include "yb/util/test_macros.h"
#include "yb/util/test_util.h"
#include "yb/util/strongly_typed_bool.h"
#include "yb/util/yb_partition.h"

using std::cout;
using std::endl;
using std::make_pair;
using std::map;
using std::string;
using std::unique_ptr;
using std::shared_ptr;
using std::make_shared;

using yb::util::TrimStr;
using yb::util::ApplyEagerLineContinuation;

using rocksdb::WriteOptions;

using namespace std::chrono_literals;

DECLARE_bool(use_docdb_aware_bloom_filter);
DECLARE_int32(max_nexts_to_avoid_seek);
DECLARE_bool(TEST_docdb_sort_weak_intents_in_tests);

#define ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(str) ASSERT_NO_FATALS(AssertDocDbDebugDumpStrEq(str))

namespace yb {
namespace docdb {

CHECKED_STATUS GetPrimitiveValue(const rocksdb::UserBoundaryValues &values,
    size_t index,
    PrimitiveValue *out);
CHECKED_STATUS GetDocHybridTime(const rocksdb::UserBoundaryValues &values, DocHybridTime *out);

YB_STRONGLY_TYPED_BOOL(InitMarkerExpired);
YB_STRONGLY_TYPED_BOOL(UseIntermediateFlushes);

class DocDBTest : public DocDBTestBase {
 protected:
  DocDBTest() {
    SeedRandom();
  }

  ~DocDBTest() override {
  }

  virtual void GetSubDoc(
      const KeyBytes& subdoc_key, SubDocument* result, bool* found_result,
      const TransactionOperationContextOpt& txn_op_context = boost::none,
      const ReadHybridTime& read_time = ReadHybridTime::Max(),
      DocHybridTime* table_tombstone_time = nullptr) = 0;

  // This is the baseline state of the database that we set up and come back to as we test various
  // operations.
  static constexpr const char *const kPredefinedDBStateDebugDumpStr = R"#(
SubDocKey(DocKey([], ["my_key_where_value_is_a_string"]), [HT{ physical: 1000 }]) -> "value1"
SubDocKey(DocKey([], ["mydockey", 123456]), [HT{ physical: 2000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_a"; HT{ physical: 2000 w: 1 }]) -> "value_a"
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b"; HT{ physical: 7000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b"; HT{ physical: 6000 }]) -> DEL
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b"; HT{ physical: 3000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b", "subkey_c"; HT{ physical: 7000 w: 1 }]) \
    -> "value_bc_prime"
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b", "subkey_c"; HT{ physical: 5000 }]) -> DEL
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b", "subkey_c"; HT{ physical: 3000 w: 1 }]) \
    -> "value_bc"
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b", "subkey_d"; HT{ physical: 3500 }]) -> \
    "value_bd"
      )#";

  static const DocKey kDocKey1;
  static const DocKey kDocKey2;
  static const KeyBytes kEncodedDocKey1;
  static const KeyBytes kEncodedDocKey2;

  void TestInsertion(
      DocPath doc_path,
      const PrimitiveValue &value,
      HybridTime hybrid_time,
      string expected_write_batch_str);

  void TestDeletion(
      DocPath doc_path,
      HybridTime hybrid_time,
      string expected_write_batch_str);

  void SetupRocksDBState(KeyBytes encoded_doc_key) {
    SubDocument root;
    SubDocument a, b, c, d, e, f, b2;

    // The test plan below:
    // Set root = {a: {1: 1, 2: 2}, b: {c: {1: 3}, d: {1: 5, 2: 6}}, u: 7}
    // Then set root.a.2 = 11
    // Then replace root.b = {e: {1: 8, 2: 9}, y: 10}
    // Then extend root.a by {1: 3, 3: 4}
    // Then Delete root.b.e.2
    // The end result should be {a: {1: 3, 2: 11, 3: 4, x: {}}, b: {e: {1: 8}, y: 10}, u: 7}

#define SET_CHILD(parent, child) parent.SetChild(PrimitiveValue(#child), std::move(child))
#define SET_VALUE(parent, key, value) parent.SetChild(PrimitiveValue(key), \
                                                      SubDocument(PrimitiveValue(value)))

    // Constructing top level document: "root"
    SET_VALUE(root, "u", "7");
    SET_VALUE(a, "1", "1");
    SET_VALUE(a, "2", "2");
    SET_VALUE(c, "1", "3");
    SET_VALUE(d, "1", "5");
    SET_VALUE(d, "2", "6");
    SET_CHILD(b, c);
    SET_CHILD(b, d);
    SET_CHILD(root, a);
    SET_CHILD(root, b);

    EXPECT_STR_EQ_VERBOSE_TRIMMED(R"#(
        {
          "a": {
            "1": "1",
            "2": "2"
          },
          "b": {
            "c": {
              "1": "3"
            },
            "d": {
              "1": "5",
              "2": "6"
            }
          },
          "u": "7"
        }
        )#", root.ToString());

    // Constructing new version of b = b2 to be inserted later.
    SET_VALUE(b2, "y", "10");
    SET_VALUE(e, "1", "8");
    SET_VALUE(e, "2", "9");
    SET_CHILD(b2, e);

    EXPECT_STR_EQ_VERBOSE_TRIMMED(R"#(
{
  "e": {
    "1": "8",
    "2": "9"
  },
  "y": "10"
}
      )#", b2.ToString());

    // Constructing a doc with which we will extend a later
    SET_VALUE(f, "1", "3");
    SET_VALUE(f, "3", "4");

    EXPECT_STR_EQ_VERBOSE_TRIMMED(R"#(
{
  "1": "3",
  "3": "4"
}
      )#", f.ToString());

#undef SET_CHILD
#undef SET_VALUE

    ASSERT_OK(InsertSubDocument(
        DocPath(encoded_doc_key), root, 1000_usec_ht));
    // The Insert above could have been an Extend with no difference in external behavior.
    // Internally however, an insert writes an extra key (with value tombstone).
    ASSERT_OK(SetPrimitive(
        DocPath(encoded_doc_key, PrimitiveValue("a"), PrimitiveValue("2")),
        Value(PrimitiveValue(11)), 2000_usec_ht));
    ASSERT_OK(InsertSubDocument(DocPath(encoded_doc_key, PrimitiveValue("b")), b2,
                                3000_usec_ht));
    ASSERT_OK(ExtendSubDocument(DocPath(encoded_doc_key, PrimitiveValue("a")), f,
                                4000_usec_ht));
    ASSERT_OK(SetPrimitive(
        DocPath(encoded_doc_key, PrimitiveValue("b"), PrimitiveValue("e"), PrimitiveValue("2")),
        Value(PrimitiveValue::kTombstone), 5000_usec_ht));
  }

  void VerifySubDocument(SubDocKey subdoc_key, HybridTime ht, string subdoc_string) {
    SubDocument doc_from_rocksdb;
    bool subdoc_found_in_rocksdb = false;

    SCOPED_TRACE("\n" + GetStackTrace(StackTraceLineFormat::CLION_CLICKABLE) + "\n" +
        DocDBDebugDumpToStr());

    // TODO(dtxn) - check both transaction and non-transaction path?
    // https://yugabyte.atlassian.net/browse/ENG-2177
    auto encoded_subdoc_key = subdoc_key.EncodeWithoutHt();
    DocHybridTime table_tombstone_time(DocHybridTime::kInvalid);
    GetSubDoc(
        encoded_subdoc_key, &doc_from_rocksdb, &subdoc_found_in_rocksdb,
        kNonTransactionalOperationContext, ReadHybridTime::SingleTime(ht), &table_tombstone_time);
    if (subdoc_string.empty()) {
      EXPECT_FALSE(subdoc_found_in_rocksdb);
      return;
    }
    EXPECT_TRUE(subdoc_found_in_rocksdb);
    EXPECT_STR_EQ_VERBOSE_TRIMMED(subdoc_string, doc_from_rocksdb.ToString());
  }

  // Tries to read some documents from the DB that is assumed to be in a state described by
  // kPredefinedDBStateDebugDumpStr, and verifies the result of those reads. Only the latest logical
  // state of documents matters for this check, so it is OK to call this after compacting previous
  // history.
  void CheckExpectedLatestDBState();

  // Checks bloom filter useful counter increment to be in range [1;expected_max_increment] and
  // table iterators number increment to be expected_num_iterators_increment.
  // Updates total_useful, total_iterators
  void CheckBloom(const int expected_max_increment, int *total_useful,
      const int expected_num_iterators_increment, int *total_iterators) {
    if (FLAGS_use_docdb_aware_bloom_filter) {
      const auto total_useful_updated =
          regular_db_options().statistics->getTickerCount(rocksdb::BLOOM_FILTER_USEFUL);
      const auto total_iterators_updated =
          regular_db_options().statistics->getTickerCount(rocksdb::NO_TABLE_CACHE_ITERATORS);
      if (expected_max_increment > 0) {
        ASSERT_GT(total_useful_updated, *total_useful);
        ASSERT_LE(total_useful_updated, *total_useful + expected_max_increment);
        *total_useful = total_useful_updated;
      } else {
        ASSERT_EQ(*total_useful, total_useful_updated);
      }
      ASSERT_EQ(*total_iterators + expected_num_iterators_increment, total_iterators_updated);
      *total_iterators = total_iterators_updated;
    }
  }

  InetAddress GetInetAddress(const std::string &strval) {
    return InetAddress(CHECK_RESULT(ParseIpAddress(strval)));
  }

  void InsertInet(const std::string strval) {
    const DocKey doc_key(PrimitiveValues("mydockey"));
    KeyBytes encoded_doc_key(doc_key.Encode());
    ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(GetInetAddress(strval))),
                           PrimitiveValue(),
                           1000_usec_ht));
  }

  // Inserts a bunch of subkeys starting with the provided doc key. It also, fills out the
  // expected_docdb_str with the expected state of DocDB after the operation.
  void AddSubKeys(const KeyBytes& encoded_doc_key, int num_subkeys, int base,
                  string* expected_docdb_str) {
    *expected_docdb_str = "";
    for (int i = 0; i < num_subkeys; i++) {
      string subkey = "subkey" + std::to_string(base + i);
      string value = "value" + std::to_string(i);
      MicrosTime hybrid_time = (i + 1) * 1000;
      ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(subkey)),
                             Value(PrimitiveValue(value)), HybridTime::FromMicros(hybrid_time)));
      *expected_docdb_str += strings::Substitute(
          R"#(SubDocKey(DocKey([], ["key"]), ["$0"; HT{ physical: $1 }]) -> "$2")#",
          subkey, hybrid_time, value);
      *expected_docdb_str += "\n";
    }
  }

  static constexpr int kNumSubKeysForCollectionsWithTTL = 3;

  void SetUpCollectionWithTTL(DocKey collection_key, UseIntermediateFlushes intermediate_flushes) {
    SubDocument subdoc;
    for (int i = 0; i < kNumSubKeysForCollectionsWithTTL; i++) {
      string key = "k" + std::to_string(i);
      string value = "v" + std::to_string(i);
      subdoc.SetChildPrimitive(PrimitiveValue(key), PrimitiveValue(value));
    }
    ASSERT_OK(InsertSubDocument(DocPath(collection_key.Encode()), subdoc, 1000_usec_ht, 10s));

    ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(Format(R"#(
        SubDocKey($0, [HT{ physical: 1000 }]) -> {}; ttl: 10.000s
        SubDocKey($0, ["k0"; HT{ physical: 1000 w: 1 }]) -> "v0"; ttl: 10.000s
        SubDocKey($0, ["k1"; HT{ physical: 1000 w: 2 }]) -> "v1"; ttl: 10.000s
        SubDocKey($0, ["k2"; HT{ physical: 1000 w: 3 }]) -> "v2"; ttl: 10.000s
        )#", collection_key.ToString()));
    if (intermediate_flushes) {
      ASSERT_OK(FlushRocksDbAndWait());
    }

    // Set separate TTLs for each element.
    for (int i = 0; i < kNumSubKeysForCollectionsWithTTL; i++) {
      SubDocument subdoc;
      string key = "k" + std::to_string(i);
      string value = "vv" + std::to_string(i);
      subdoc.SetChildPrimitive(PrimitiveValue(key), PrimitiveValue(value));
      ASSERT_OK(ExtendSubDocument(
          DocPath(collection_key.Encode()), subdoc, 1100_usec_ht,
          MonoDelta::FromSeconds(20 + i)));
      if (intermediate_flushes) {
        ASSERT_OK(FlushRocksDbAndWait());
      }
    }

    // Add new keys as well.
    for (int i = kNumSubKeysForCollectionsWithTTL; i < kNumSubKeysForCollectionsWithTTL * 2; i++) {
      SubDocument subdoc;
      string key = "k" + std::to_string(i);
      string value = "vv" + std::to_string(i);
      subdoc.SetChildPrimitive(PrimitiveValue(key), PrimitiveValue(value));
      ASSERT_OK(ExtendSubDocument(
          DocPath(collection_key.Encode()), subdoc, 1100_usec_ht,
          MonoDelta::FromSeconds(20 + i)));
      if (intermediate_flushes) {
        ASSERT_OK(FlushRocksDbAndWait());
      }
    }
  }

  string ExpectedDebugDumpForCollectionWithTTL(DocKey collection_key,
                                               InitMarkerExpired init_marker_expired) {
    // The "file ..." comments below are for the case of intermediate_flushes = true above.
    const string result_template = init_marker_expired ?
        // After the init marker expires, we should not see a tombstone for it. We do not replace
        // timed-out collection init markers with tombstones on minor compactions, because that
        // could hide keys that
        R"#(
            SubDocKey($0, ["k0"; HT{ physical: 1100 }]) -> "vv0"; ttl: 20.000s
            SubDocKey($0, ["k1"; HT{ physical: 1100 }]) -> "vv1"; ttl: 21.000s
            SubDocKey($0, ["k2"; HT{ physical: 1100 }]) -> "vv2"; ttl: 22.000s
            SubDocKey($0, ["k3"; HT{ physical: 1100 }]) -> "vv3"; ttl: 23.000s
            SubDocKey($0, ["k4"; HT{ physical: 1100 }]) -> "vv4"; ttl: 24.000s
            SubDocKey($0, ["k5"; HT{ physical: 1100 }]) -> "vv5"; ttl: 25.000s
        )#" : R"#(
            SubDocKey($0, [HT{ physical: 1000 }]) -> {}; ttl: 10.000s               // file 1
            SubDocKey($0, ["k0"; HT{ physical: 1100 }]) -> "vv0"; ttl: 20.000s      // file 2
            SubDocKey($0, ["k0"; HT{ physical: 1000 w: 1 }]) -> "v0"; ttl: 10.000s  // file 1
            SubDocKey($0, ["k1"; HT{ physical: 1100 }]) -> "vv1"; ttl: 21.000s      // file 3
            SubDocKey($0, ["k1"; HT{ physical: 1000 w: 2 }]) -> "v1"; ttl: 10.000s  // file 1
            SubDocKey($0, ["k2"; HT{ physical: 1100 }]) -> "vv2"; ttl: 22.000s      // file 4
            SubDocKey($0, ["k2"; HT{ physical: 1000 w: 3 }]) -> "v2"; ttl: 10.000s  // file 1
            SubDocKey($0, ["k3"; HT{ physical: 1100 }]) -> "vv3"; ttl: 23.000s      // file 5
            SubDocKey($0, ["k4"; HT{ physical: 1100 }]) -> "vv4"; ttl: 24.000s      // file 6
            SubDocKey($0, ["k5"; HT{ physical: 1100 }]) -> "vv5"; ttl: 25.000s      // file 7
        )#";
    return Format(result_template, collection_key.ToString());
  }

  void InitializeCollection(const std::string& key_string,
                            std::string* val_string,
                            vector<HybridTime>::iterator* time_iter,
                            std::set<std::pair<string, string>>* docdb_dump) {
    SubDocument subdoc;
    DocKey collection_key(PrimitiveValues(key_string));

    for (int i = 0; i < kNumSubKeysForCollectionsWithTTL; i++) {
      string key = "sk" + std::to_string(i);
      subdoc.SetChildPrimitive(PrimitiveValue(key), PrimitiveValue(*val_string));
      (*val_string)[1]++;
    }

    ASSERT_OK(InsertSubDocument(DocPath(collection_key.Encode()), subdoc, **time_iter));
    ++*time_iter;

    SubDocument new_subdoc;
    // Add new keys as well.
    for (int i = kNumSubKeysForCollectionsWithTTL / 2;
         i < 3 * kNumSubKeysForCollectionsWithTTL / 2; i++) {
      string key = "sk" + std::to_string(i);
      new_subdoc.SetChildPrimitive(PrimitiveValue(key), PrimitiveValue(*val_string));
      (*val_string)[1]++;
    }
    ASSERT_OK(ExtendSubDocument(
      DocPath(collection_key.Encode()), new_subdoc, **time_iter));
    ++*time_iter;

  }
};

void GetSubDocQl(
      const DocDB& doc_db, const KeyBytes& subdoc_key, SubDocument* result, bool* found_result,
      const TransactionOperationContextOpt& txn_op_context, const ReadHybridTime& read_time,
      DocHybridTime* table_tombstone_time) {
  GetSubDocumentData data = { subdoc_key, result, found_result };
  data.table_tombstone_time = table_tombstone_time;
  ASSERT_OK(GetSubDocument(
      doc_db, data, rocksdb::kDefaultQueryId,
      txn_op_context, CoarseTimePoint::max() /* deadline */,
      read_time));
}

void GetSubDocRedis(
      const DocDB& doc_db, const KeyBytes& subdoc_key, SubDocument* result, bool* found_result,
      const TransactionOperationContextOpt& txn_op_context, const ReadHybridTime& read_time,
      DocHybridTime* table_tombstone_time) {
  GetRedisSubDocumentData data = { subdoc_key, result, found_result };
  ASSERT_OK(GetRedisSubDocument(
      doc_db, data, rocksdb::kDefaultQueryId,
      txn_op_context, CoarseTimePoint::max() /* deadline */,
      read_time));
}

// The list of types we want to test.
YB_DEFINE_ENUM(TestDocDb, (kQlReader)(kRedisReader));

class DocDBTestWrapper : public DocDBTest, public testing::WithParamInterface<TestDocDb>  {
 public:
  void GetSubDoc(
      const KeyBytes& subdoc_key, SubDocument* result, bool* found_result,
      const TransactionOperationContextOpt& txn_op_context = boost::none,
      const ReadHybridTime& read_time = ReadHybridTime::Max(),
      DocHybridTime* table_tombstone_time = nullptr) override {
    switch (GetParam()) {
      case TestDocDb::kQlReader: {
        GetSubDocQl(
            doc_db(), subdoc_key, result, found_result, txn_op_context, read_time,
            table_tombstone_time);
        break;
      }
      case TestDocDb::kRedisReader: {
        GetSubDocRedis(
            doc_db(), subdoc_key, result, found_result, txn_op_context, read_time,
            table_tombstone_time);
        break;
      }
    }
  }
};

INSTANTIATE_TEST_CASE_P(DocDBTests,
                        DocDBTestWrapper,
                        testing::Values(TestDocDb::kQlReader, TestDocDb::kRedisReader));

class DocDBTestQl : public DocDBTest {
 public:
  void GetSubDoc(
      const KeyBytes& subdoc_key, SubDocument* result, bool* found_result,
      const TransactionOperationContextOpt& txn_op_context = boost::none,
      const ReadHybridTime& read_time = ReadHybridTime::Max(),
      DocHybridTime* table_tombstone_time = nullptr) override {
    GetSubDocQl(
        doc_db(), subdoc_key, result, found_result, txn_op_context, read_time,
        table_tombstone_time);
  }
};

class DocDBTestRedis : public DocDBTest {
 public:
  void GetSubDoc(
      const KeyBytes& subdoc_key, SubDocument* result, bool* found_result,
      const TransactionOperationContextOpt& txn_op_context = boost::none,
      const ReadHybridTime& read_time = ReadHybridTime::Max(),
      DocHybridTime* table_tombstone_time = nullptr) override {
    GetSubDocRedis(
        doc_db(), subdoc_key, result, found_result, txn_op_context, read_time,
        table_tombstone_time);
  }
};

// Static constant initialization should be completely independent (cannot initialize one using the
// other).
const DocKey DocDBTest::kDocKey1(PrimitiveValues("row1", 11111));
const DocKey DocDBTest::kDocKey2(PrimitiveValues("row2", 22222));
const KeyBytes DocDBTest::kEncodedDocKey1(DocKey(PrimitiveValues("row1", 11111)).Encode());
const KeyBytes DocDBTest::kEncodedDocKey2(DocKey(PrimitiveValues("row2", 22222)).Encode());

void DocDBTest::TestInsertion(
    const DocPath doc_path,
    const PrimitiveValue &value,
    HybridTime hybrid_time,
    string expected_write_batch_str) {
  auto dwb = MakeDocWriteBatch();
  // Set write id to zero on the write path.
  ASSERT_OK(dwb.SetPrimitive(doc_path, value));
  ASSERT_OK(WriteToRocksDB(dwb, hybrid_time));
  string dwb_str;
  ASSERT_OK(FormatDocWriteBatch(dwb, &dwb_str));
  EXPECT_STR_EQ_VERBOSE_TRIMMED(ApplyEagerLineContinuation(expected_write_batch_str),
      dwb_str);
}

void DocDBTest::TestDeletion(
    DocPath doc_path,
    HybridTime hybrid_time,
    string expected_write_batch_str) {
  auto dwb = MakeDocWriteBatch();
  // Set write id to zero on the write path.
  ASSERT_OK(dwb.DeleteSubDoc(doc_path));
  ASSERT_OK(WriteToRocksDB(dwb, hybrid_time));
  string dwb_str;
  ASSERT_OK(FormatDocWriteBatch(dwb, &dwb_str));
  EXPECT_STR_EQ_VERBOSE_TRIMMED(ApplyEagerLineContinuation(expected_write_batch_str),
      dwb_str);
}

void DocDBTest::CheckExpectedLatestDBState() {
  const SubDocKey subdoc_key(DocKey(PrimitiveValues("mydockey", 123456)));

  SubDocument subdoc;
  bool doc_found = false;
  // TODO(dtxn) - check both transaction and non-transaction path?
  auto encoded_subdoc_key = subdoc_key.EncodeWithoutHt();
  GetSubDoc(
      encoded_subdoc_key, &subdoc, &doc_found,
      kNonTransactionalOperationContext);
  ASSERT_TRUE(doc_found);
  ASSERT_STR_EQ_VERBOSE_TRIMMED(
      R"#(
{
  "subkey_a": "value_a",
  "subkey_b": {
    "subkey_c": "value_bc_prime"
  }
}
      )#",
      subdoc.ToString()
  );
}

// ------------------------------------------------------------------------------------------------

TEST_P(DocDBTestWrapper, DocPathTest) {
  DocKey doc_key(PrimitiveValues("mydockey", 10, "mydockey", 20));
  DocPath doc_path(doc_key.Encode(), "first_subkey", 123);
  ASSERT_EQ(2, doc_path.num_subkeys());
  ASSERT_EQ("\"first_subkey\"", doc_path.subkey(0).ToString());
  ASSERT_EQ("123", doc_path.subkey(1).ToString());
}

TEST_P(DocDBTestWrapper, KeyAsEmptyObjectIsNotMasked) {
  const DocKey doc_key(PrimitiveValues(DocKeyHash(1234)));
  KeyBytes encoded_doc_key(doc_key.Encode());
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key), PrimitiveValue::kObject, 252_usec_ht));
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key, PrimitiveValue()), PrimitiveValue::kObject, 617_usec_ht));
  ASSERT_OK(DeleteSubDoc(
      DocPath(encoded_doc_key, PrimitiveValue(), PrimitiveValue(ValueType::kFalse)), 675_usec_ht));
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key, PrimitiveValue(), PrimitiveValue(ValueType::kFalse)),
      PrimitiveValue(12345), 617_usec_ht));
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key, "later"), PrimitiveValue(1), 336_usec_ht));
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], [1234]), [HT{ physical: 252 }]) -> {}
      SubDocKey(DocKey([], [1234]), [null; HT{ physical: 617 }]) -> {}
      SubDocKey(DocKey([], [1234]), [null, false; HT{ physical: 675 }]) -> DEL
      SubDocKey(DocKey([], [1234]), [null, false; HT{ physical: 617 }]) -> 12345
      SubDocKey(DocKey([], [1234]), ["later"; HT{ physical: 336 }]) -> 1
      )#");
  VerifySubDocument(SubDocKey(doc_key), 4000_usec_ht,
                    R"#(
{
  null: {},
  "later": 1
}
      )#");
}

TEST_P(DocDBTestWrapper, NullChildObjectShouldMaskValues) {
  const DocKey doc_key(PrimitiveValues("mydockey", 123456));
  KeyBytes encoded_doc_key(doc_key.Encode());
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key), PrimitiveValue::kObject, 1000_usec_ht));
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key, "obj"), PrimitiveValue::kObject, 2000_usec_ht));
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key, "obj", "key"), PrimitiveValue("value"), 2000_usec_ht));
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key, "obj"), PrimitiveValue(ValueType::kNullHigh), 3000_usec_ht));
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["mydockey", 123456]), [HT{ physical: 1000 }]) -> {}
      SubDocKey(DocKey([], ["mydockey", 123456]), ["obj"; HT{ physical: 3000 }]) -> null
      SubDocKey(DocKey([], ["mydockey", 123456]), ["obj"; HT{ physical: 2000 }]) -> {}
      SubDocKey(DocKey([], ["mydockey", 123456]), ["obj", "key"; HT{ physical: 2000 }]) -> "value"
      )#");
  VerifySubDocument(SubDocKey(doc_key), 4000_usec_ht,
                    R"#(
{
  "obj": null
}
      )#");
}

TEST_F(DocDBTestQl, ColocatedTableTombstoneTest) {
  constexpr PgTableOid pgtable_id(0x4001);
  DocKey doc_key_1(PrimitiveValues("mydockey", 123456));
  doc_key_1.set_pgtable_id(pgtable_id);
  DocKey doc_key_2(PrimitiveValues("mydockey", 789123));
  doc_key_2.set_pgtable_id(pgtable_id);
  ASSERT_OK(SetPrimitive(
      doc_key_1.Encode(), PrimitiveValue(1), 1000_usec_ht));
  ASSERT_OK(SetPrimitive(
      doc_key_2.Encode(), PrimitiveValue(2), 1000_usec_ht));

  DocKey doc_key_table;
  doc_key_table.set_pgtable_id(pgtable_id);
  ASSERT_OK(DeleteSubDoc(
      doc_key_table.Encode(), 2000_usec_ht));

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey(PgTableId=16385, [], []), [HT{ physical: 2000 }]) -> DEL
      SubDocKey(DocKey(PgTableId=16385, [], ["mydockey", 123456]), [HT{ physical: 1000 }]) -> 1
      SubDocKey(DocKey(PgTableId=16385, [], ["mydockey", 789123]), [HT{ physical: 1000 }]) -> 2
      )#");
  VerifySubDocument(SubDocKey(doc_key_1), 4000_usec_ht, "");
  VerifySubDocument(SubDocKey(doc_key_1), 1500_usec_ht, "1");
}

TEST_P(DocDBTestWrapper, HistoryCompactionFirstRowHandlingRegression) {
  // A regression test for a bug in an initial version of compaction cleanup.
  const DocKey doc_key(PrimitiveValues("mydockey", 123456));
  KeyBytes encoded_doc_key(doc_key.Encode());
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key), PrimitiveValue::kObject, 1000_usec_ht));
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key, "subkey1"),
      PrimitiveValue("value1"),
      1000_usec_ht));
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key, "subkey1"),
      PrimitiveValue("value2"),
      2000_usec_ht));
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key, "subkey1"),
      PrimitiveValue("value3"),
      3000_usec_ht));
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key), PrimitiveValue::kObject, 4000_usec_ht));
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["mydockey", 123456]), [HT{ physical: 4000 }]) -> {}
      SubDocKey(DocKey([], ["mydockey", 123456]), [HT{ physical: 1000 }]) -> {}
      SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey1"; HT{ physical: 3000 }]) -> "value3"
      SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey1"; HT{ physical: 2000 }]) -> "value2"
      SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey1"; HT{ physical: 1000 }]) -> "value1"
      )#");
  FullyCompactHistoryBefore(3500_usec_ht);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["mydockey", 123456]), [HT{ physical: 4000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), [HT{ physical: 1000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey1"; HT{ physical: 3000 }]) -> "value3"
      )#");
}

TEST_P(DocDBTestWrapper, SetPrimitiveQL) {
  const DocKey doc_key(PrimitiveValues("mydockey", 123456));
  SetupRocksDBState(doc_key.Encode());
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["mydockey", 123456]), [HT{ physical: 1000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["a", "1"; HT{ physical: 4000 }]) -> "3"
SubDocKey(DocKey([], ["mydockey", 123456]), ["a", "1"; HT{ physical: 1000 w: 1 }]) -> "1"
SubDocKey(DocKey([], ["mydockey", 123456]), ["a", "2"; HT{ physical: 2000 }]) -> 11
SubDocKey(DocKey([], ["mydockey", 123456]), ["a", "2"; HT{ physical: 1000 w: 2 }]) -> "2"
SubDocKey(DocKey([], ["mydockey", 123456]), ["a", "3"; HT{ physical: 4000 w: 1 }]) -> "4"
SubDocKey(DocKey([], ["mydockey", 123456]), ["b"; HT{ physical: 3000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["b", "c", "1"; HT{ physical: 1000 w: 3 }]) -> "3"
SubDocKey(DocKey([], ["mydockey", 123456]), ["b", "d", "1"; HT{ physical: 1000 w: 4 }]) -> "5"
SubDocKey(DocKey([], ["mydockey", 123456]), ["b", "d", "2"; HT{ physical: 1000 w: 5 }]) -> "6"
SubDocKey(DocKey([], ["mydockey", 123456]), ["b", "e", "1"; HT{ physical: 3000 w: 1 }]) -> "8"
SubDocKey(DocKey([], ["mydockey", 123456]), ["b", "e", "2"; HT{ physical: 5000 }]) -> DEL
SubDocKey(DocKey([], ["mydockey", 123456]), ["b", "e", "2"; HT{ physical: 3000 w: 2 }]) -> "9"
SubDocKey(DocKey([], ["mydockey", 123456]), ["b", "y"; HT{ physical: 3000 w: 3 }]) -> "10"
SubDocKey(DocKey([], ["mydockey", 123456]), ["u"; HT{ physical: 1000 w: 6 }]) -> "7"
     )#");
}

// This tests GetSubDocument without init markers. Basic Test tests with init markers.
TEST_P(DocDBTestWrapper, GetSubDocumentTest) {
  const DocKey doc_key(PrimitiveValues("mydockey", 123456));
  SetupRocksDBState(doc_key.Encode());

  // We will test the state of the entire document after every operation, using timestamps
  // 500, 1500, 2500, 3500, 4500, 5500.

  VerifySubDocument(SubDocKey(doc_key), 500_usec_ht, "");

  VerifySubDocument(SubDocKey(doc_key), 1500_usec_ht,
                    R"#(
{
  "a": {
    "1": "1",
    "2": "2"
  },
  "b": {
    "c": {
      "1": "3"
    },
    "d": {
      "1": "5",
      "2": "6"
    }
  },
  "u": "7"
}
      )#");

  VerifySubDocument(SubDocKey(doc_key), 2500_usec_ht,
                    R"#(
{
  "a": {
    "1": "1",
    "2": 11
  },
  "b": {
    "c": {
      "1": "3"
    },
    "d": {
      "1": "5",
      "2": "6"
    }
  },
  "u": "7"
}
      )#");

  VerifySubDocument(SubDocKey(doc_key), 3500_usec_ht,
                    R"#(
{
  "a": {
    "1": "1",
    "2": 11
  },
  "b": {
    "e": {
      "1": "8",
      "2": "9"
    },
    "y": "10"
  },
  "u": "7"
}
      )#");

  VerifySubDocument(SubDocKey(doc_key), 4500_usec_ht,
                    R"#(
{
  "a": {
    "1": "3",
    "2": 11,
    "3": "4"
  },
  "b": {
    "e": {
      "1": "8",
      "2": "9"
    },
    "y": "10"
  },
  "u": "7"
}
      )#");

  VerifySubDocument(SubDocKey(doc_key), 5500_usec_ht,
                    R"#(
{
  "a": {
    "1": "3",
    "2": 11,
    "3": "4"
  },
  "b": {
    "e": {
      "1": "8"
    },
    "y": "10"
  },
  "u": "7"
}
      )#");

  // Test the evolution of SubDoc root.b at various timestamps.

  VerifySubDocument(SubDocKey(doc_key, PrimitiveValue("b")), 500_usec_ht, "");

  VerifySubDocument(SubDocKey(doc_key, PrimitiveValue("b")), 2500_usec_ht,
                    R"#(
{
  "c": {
    "1": "3"
  },
  "d": {
    "1": "5",
    "2": "6"
  }
}
      )#");

  VerifySubDocument(SubDocKey(doc_key, PrimitiveValue("b")), 3500_usec_ht,
                    R"#(
{
  "e": {
    "1": "8",
    "2": "9"
  },
  "y": "10"
}
      )#");

  VerifySubDocument(SubDocKey(doc_key, PrimitiveValue("b")), 5500_usec_ht,
                    R"#(
{
  "e": {
    "1": "8"
  },
  "y": "10"
}
      )#");

  VerifySubDocument(SubDocKey(
      doc_key, PrimitiveValue("b"), PrimitiveValue("d")), 10000_usec_ht, "");

  VerifySubDocument(SubDocKey(doc_key, PrimitiveValue("b"), PrimitiveValue("d")),
                    2500_usec_ht,
                    R"#(
  {
    "1": "5",
    "2": "6"
  }
        )#");

}

TEST_P(DocDBTestWrapper, ListInsertAndGetTest) {
  SubDocument parent;
  SubDocument list({PrimitiveValue(10), PrimitiveValue(2)});
  DocKey doc_key(PrimitiveValues("list_test", 231));
  KeyBytes encoded_doc_key = doc_key.Encode();
  parent.SetChild(PrimitiveValue("other"), SubDocument(PrimitiveValue("other_value")));
  parent.SetChild(PrimitiveValue("list2"), SubDocument(list));
  ASSERT_OK(InsertSubDocument(DocPath(encoded_doc_key), parent, HybridTime(100)));

  // GetSubDocument Doesn't know that this is an array so it is returned as an object for now.
  VerifySubDocument(SubDocKey(doc_key), HybridTime(250),
      R"#(
  {
    "list2": {
      ArrayIndex(1): 10,
      ArrayIndex(2): 2
    },
    "other": "other_value"
  }
        )#");

  ASSERT_OK(ExtendSubDocument(DocPath(encoded_doc_key, PrimitiveValue("list1")),
      SubDocument({PrimitiveValue(1), PrimitiveValue("3"), PrimitiveValue(2), PrimitiveValue(2)}),
      HybridTime(200)));

  VerifySubDocument(SubDocKey(doc_key), HybridTime(250),
      R"#(
  {
    "list1": {
      ArrayIndex(3): 1,
      ArrayIndex(4): "3",
      ArrayIndex(5): 2,
      ArrayIndex(6): 2
    },
    "list2": {
      ArrayIndex(1): 10,
      ArrayIndex(2): 2
    },
    "other": "other_value"
  }
        )#");

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["list_test", 231]), [HT{ physical: 0 logical: 100 }]) -> {}
SubDocKey(DocKey([], ["list_test", 231]), ["list1", ArrayIndex(3); \
    HT{ physical: 0 logical: 200 }]) -> 1
SubDocKey(DocKey([], ["list_test", 231]), ["list1", ArrayIndex(4); \
    HT{ physical: 0 logical: 200 w: 1 }]) -> "3"
SubDocKey(DocKey([], ["list_test", 231]), ["list1", ArrayIndex(5); \
    HT{ physical: 0 logical: 200 w: 2 }]) -> 2
SubDocKey(DocKey([], ["list_test", 231]), ["list1", ArrayIndex(6); \
    HT{ physical: 0 logical: 200 w: 3 }]) -> 2
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(1); \
    HT{ physical: 0 logical: 100 w: 1 }]) -> 10
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(2); \
    HT{ physical: 0 logical: 100 w: 2 }]) -> 2
SubDocKey(DocKey([], ["list_test", 231]), ["other"; \
    HT{ physical: 0 logical: 100 w: 3 }]) -> "other_value"
        )#");
  ASSERT_OK(ExtendList(DocPath(encoded_doc_key, PrimitiveValue("list2")),
      SubDocument({PrimitiveValue(5), PrimitiveValue(2)}, ListExtendOrder::PREPEND_BLOCK),
      HybridTime(300)));
  ASSERT_OK(ExtendList(DocPath(encoded_doc_key, PrimitiveValue("list2")),
      SubDocument({PrimitiveValue(7), PrimitiveValue(4)}, ListExtendOrder::APPEND),
      HybridTime(400)));

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["list_test", 231]), [HT{ physical: 0 logical: 100 }]) -> {}
SubDocKey(DocKey([], ["list_test", 231]), ["list1", ArrayIndex(3); \
    HT{ physical: 0 logical: 200 }]) -> 1
SubDocKey(DocKey([], ["list_test", 231]), ["list1", ArrayIndex(4); \
    HT{ physical: 0 logical: 200 w: 1 }]) -> "3"
SubDocKey(DocKey([], ["list_test", 231]), ["list1", ArrayIndex(5); \
    HT{ physical: 0 logical: 200 w: 2 }]) -> 2
SubDocKey(DocKey([], ["list_test", 231]), ["list1", ArrayIndex(6); \
    HT{ physical: 0 logical: 200 w: 3 }]) -> 2
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(-8); \
    HT{ physical: 0 logical: 300 w: 1 }]) -> 5
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(-7); \
    HT{ physical: 0 logical: 300 }]) -> 2
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(1); \
    HT{ physical: 0 logical: 100 w: 1 }]) -> 10
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(2); \
    HT{ physical: 0 logical: 100 w: 2 }]) -> 2
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(9); \
    HT{ physical: 0 logical: 400 }]) -> 7
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(10); \
    HT{ physical: 0 logical: 400 w: 1 }]) -> 4
SubDocKey(DocKey([], ["list_test", 231]), ["other"; \
    HT{ physical: 0 logical: 100 w: 3 }]) -> "other_value"
        )#");

  VerifySubDocument(SubDocKey(doc_key), HybridTime(150),
      R"#(
  {
    "list2": {
      ArrayIndex(1): 10,
      ArrayIndex(2): 2
    },
    "other": "other_value"
  }
        )#");

  VerifySubDocument(SubDocKey(doc_key), HybridTime(450),
      R"#(
  {
    "list1": {
      ArrayIndex(3): 1,
      ArrayIndex(4): "3",
      ArrayIndex(5): 2,
      ArrayIndex(6): 2
    },
    "list2": {
      ArrayIndex(-8): 5,
      ArrayIndex(-7): 2,
      ArrayIndex(1): 10,
      ArrayIndex(2): 2,
      ArrayIndex(9): 7,
      ArrayIndex(10): 4
    },
    "other": "other_value"
  }
        )#");

  vector<int> indexes = {2, 4};
  ReadHybridTime read_ht;
  read_ht.read = HybridTime(460);
  vector<SubDocument> values = {
      SubDocument(PrimitiveValue::kTombstone), SubDocument(PrimitiveValue(17))};
  ASSERT_OK(ReplaceInList(DocPath(encoded_doc_key, PrimitiveValue("list2")),
    indexes, values, read_ht, HybridTime(500), rocksdb::kDefaultQueryId));

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["list_test", 231]), [HT{ physical: 0 logical: 100 }]) -> {}
SubDocKey(DocKey([], ["list_test", 231]), ["list1", ArrayIndex(3); \
    HT{ physical: 0 logical: 200 }]) -> 1
SubDocKey(DocKey([], ["list_test", 231]), ["list1", ArrayIndex(4); \
    HT{ physical: 0 logical: 200 w: 1 }]) -> "3"
SubDocKey(DocKey([], ["list_test", 231]), ["list1", ArrayIndex(5); \
    HT{ physical: 0 logical: 200 w: 2 }]) -> 2
SubDocKey(DocKey([], ["list_test", 231]), ["list1", ArrayIndex(6); \
    HT{ physical: 0 logical: 200 w: 3 }]) -> 2
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(-8); \
    HT{ physical: 0 logical: 300 w: 1 }]) -> 5
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(-7); \
    HT{ physical: 0 logical: 500 }]) -> DEL
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(-7); \
    HT{ physical: 0 logical: 300 }]) -> 2
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(1); \
    HT{ physical: 0 logical: 100 w: 1 }]) -> 10
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(2); \
    HT{ physical: 0 logical: 500 w: 1 }]) -> 17
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(2); \
    HT{ physical: 0 logical: 100 w: 2 }]) -> 2
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(9); \
    HT{ physical: 0 logical: 400 }]) -> 7
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(10); \
    HT{ physical: 0 logical: 400 w: 1 }]) -> 4
SubDocKey(DocKey([], ["list_test", 231]), ["other"; \
    HT{ physical: 0 logical: 100 w: 3 }]) -> "other_value"
        )#");

  VerifySubDocument(SubDocKey(doc_key), HybridTime(550),
      R"#(
  {
    "list1": {
      ArrayIndex(3): 1,
      ArrayIndex(4): "3",
      ArrayIndex(5): 2,
      ArrayIndex(6): 2
    },
    "list2": {
      ArrayIndex(-8): 5,
      ArrayIndex(1): 10,
      ArrayIndex(2): 17,
      ArrayIndex(9): 7,
      ArrayIndex(10): 4
    },
    "other": "other_value"
  }
        )#");

  SubDocKey sub_doc_key(doc_key, PrimitiveValue("list3"));
  KeyBytes encoded_sub_doc_key = sub_doc_key.Encode();
  SubDocument list3({PrimitiveValue(31), PrimitiveValue(32)});

  ASSERT_OK(InsertSubDocument(DocPath(encoded_sub_doc_key), list3, HybridTime(100)));

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["list_test", 231]), [HT{ physical: 0 logical: 100 }]) -> {}
SubDocKey(DocKey([], ["list_test", 231]), ["list1", ArrayIndex(3); \
    HT{ physical: 0 logical: 200 }]) -> 1
SubDocKey(DocKey([], ["list_test", 231]), ["list1", ArrayIndex(4); \
    HT{ physical: 0 logical: 200 w: 1 }]) -> "3"
SubDocKey(DocKey([], ["list_test", 231]), ["list1", ArrayIndex(5); \
    HT{ physical: 0 logical: 200 w: 2 }]) -> 2
SubDocKey(DocKey([], ["list_test", 231]), ["list1", ArrayIndex(6); \
    HT{ physical: 0 logical: 200 w: 3 }]) -> 2
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(-8); \
    HT{ physical: 0 logical: 300 w: 1 }]) -> 5
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(-7); \
    HT{ physical: 0 logical: 500 }]) -> DEL
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(-7); \
    HT{ physical: 0 logical: 300 }]) -> 2
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(1); \
    HT{ physical: 0 logical: 100 w: 1 }]) -> 10
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(2); \
    HT{ physical: 0 logical: 500 w: 1 }]) -> 17
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(2); \
    HT{ physical: 0 logical: 100 w: 2 }]) -> 2
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(9); \
    HT{ physical: 0 logical: 400 }]) -> 7
SubDocKey(DocKey([], ["list_test", 231]), ["list2", ArrayIndex(10); \
    HT{ physical: 0 logical: 400 w: 1 }]) -> 4
SubDocKey(DocKey([], ["list_test", 231]), ["list3"; HT{ physical: 0 logical: 100 }]) -> []
SubDocKey(DocKey([], ["list_test", 231]), ["list3", ArrayIndex(11); \
    HT{ physical: 0 logical: 100 w: 1 }]) -> 31
SubDocKey(DocKey([], ["list_test", 231]), ["list3", ArrayIndex(12); \
    HT{ physical: 0 logical: 100 w: 2 }]) -> 32
SubDocKey(DocKey([], ["list_test", 231]), ["other"; \
    HT{ physical: 0 logical: 100 w: 3 }]) -> "other_value"
        )#");

  VerifySubDocument(SubDocKey(doc_key), HybridTime(550),
      R"#(
  {
    "list1": {
      ArrayIndex(3): 1,
      ArrayIndex(4): "3",
      ArrayIndex(5): 2,
      ArrayIndex(6): 2
    },
    "list2": {
      ArrayIndex(-8): 5,
      ArrayIndex(1): 10,
      ArrayIndex(2): 17,
      ArrayIndex(9): 7,
      ArrayIndex(10): 4
    },
    "list3": {
      ArrayIndex(11): 31,
      ArrayIndex(12): 32
    },
    "other": "other_value"
  }
        )#");
}

TEST_P(DocDBTestWrapper, ExpiredValueCompactionTest) {
  const DocKey doc_key(PrimitiveValues("k1"));
  const MonoDelta one_ms = 1ms;
  const MonoDelta two_ms = 2ms;
  const HybridTime t0 = 1000_usec_ht;
  HybridTime t1 = server::HybridClock::AddPhysicalTimeToHybridTime(t0, two_ms);
  HybridTime t2 = server::HybridClock::AddPhysicalTimeToHybridTime(t1, two_ms);
  KeyBytes encoded_doc_key(doc_key.Encode());
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue("s1")),
      Value(PrimitiveValue("v11"), one_ms), t0));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue("s1")),
      PrimitiveValue("v14"), t2));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue("s2")),
      Value(PrimitiveValue("v21"), 3ms), t0));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue("s2")),
      PrimitiveValue("v24"), t2));

  // Note: HT{ physical: 1000 } + 4ms = HT{ physical: 5000 }
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["k1"]), ["s1"; HT{ physical: 5000 }]) -> "v14"
      SubDocKey(DocKey([], ["k1"]), ["s1"; HT{ physical: 1000 }]) -> "v11"; ttl: 0.001s
      SubDocKey(DocKey([], ["k1"]), ["s2"; HT{ physical: 5000 }]) -> "v24"
      SubDocKey(DocKey([], ["k1"]), ["s2"; HT{ physical: 1000 }]) -> "v21"; ttl: 0.003s
      )#");
  FullyCompactHistoryBefore(t1);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k1"]), ["s1"; HT{ physical: 5000 }]) -> "v14"
SubDocKey(DocKey([], ["k1"]), ["s2"; HT{ physical: 5000 }]) -> "v24"
SubDocKey(DocKey([], ["k1"]), ["s2"; HT{ physical: 1000 }]) -> "v21"; ttl: 0.003s
      )#");
}

TEST_P(DocDBTestWrapper, GetDocTwoLists) {
  SubDocument parent;
  SubDocument list1({PrimitiveValue(10), PrimitiveValue(2)});
  DocKey doc_key(PrimitiveValues("list_test", 231));

  KeyBytes encoded_doc_key = doc_key.Encode();
  parent.SetChild(PrimitiveValue("list1"), SubDocument(list1));
  ASSERT_OK(InsertSubDocument(DocPath(encoded_doc_key), parent, HybridTime(100)));

  SubDocKey sub_doc_key(doc_key, PrimitiveValue("list2"));
  KeyBytes encoded_sub_doc_key = sub_doc_key.Encode();
  SubDocument list2({PrimitiveValue(31), PrimitiveValue(32)});

  ASSERT_OK(InsertSubDocument(DocPath(encoded_sub_doc_key), list2, HybridTime(100)));

  VerifySubDocument(SubDocKey(doc_key), HybridTime(550),
      R"#(
  {
    "list1": {
      ArrayIndex(1): 10,
      ArrayIndex(2): 2
    },
    "list2": {
      ArrayIndex(3): 31,
      ArrayIndex(4): 32
    }
  }
      )#");
}


// Compaction testing with TTL merge records for generic Redis collections.
// Observe that because only collection-level merge records are supported,
// all tests begin with initializing a vanilla collection and adding TTL over it.
TEST_P(DocDBTestWrapper, RedisCollectionTTLCompactionTest) {
  const MonoDelta one_ms = 1ms;
  string key_string = "k0";
  string val_string = "v0";
  int n_times = 35;
  vector<HybridTime> t(n_times);
  t[0] = 1000_usec_ht;
  for (int i = 1; i < n_times; ++i) {
    t[i] = server::HybridClock::AddPhysicalTimeToHybridTime(t[i-1], one_ms);
  }

  std::set<std::pair<string, string>> docdb_dump;
  auto time_iter = t.begin();

  // Stack 1
  InitializeCollection(key_string, &val_string, &time_iter, &docdb_dump);
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
    Value(PrimitiveValue(ValueType::kTombstone)), *time_iter));
  ++time_iter;
  InitializeCollection(key_string, &val_string, &time_iter, &docdb_dump);
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
    Value(PrimitiveValue(ValueType::kObject), 21ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
                         *time_iter));
  ++time_iter;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
    Value(PrimitiveValue(ValueType::kObject), 9ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
                         *time_iter));
  time_iter = t.begin();
  ++key_string[1];

  // Stack 2
  InitializeCollection(key_string, &val_string, &time_iter, &docdb_dump);
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
    Value(PrimitiveValue(ValueType::kObject), 18ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
                         *time_iter));
  ++time_iter;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
    Value(PrimitiveValue(ValueType::kObject), 15ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
                         *time_iter));
  ++time_iter;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
    Value(PrimitiveValue(ValueType::kTombstone)), *time_iter));
  time_iter = t.begin();
  ++key_string[1];

  // Stack 3
  InitializeCollection(key_string, &val_string, &time_iter, &docdb_dump);
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
    Value(PrimitiveValue(ValueType::kObject), 15ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
                         *time_iter));
  ++time_iter;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
    Value(PrimitiveValue(ValueType::kObject), 18ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
                         *time_iter));
  ++time_iter;
  InitializeCollection(key_string, &val_string, &time_iter, &docdb_dump);
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
    Value(PrimitiveValue(ValueType::kObject), 21ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
                         *time_iter));
  ++time_iter;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
    Value(PrimitiveValue(ValueType::kObject), 9ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
                         *time_iter));
  ++time_iter;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
    Value(PrimitiveValue(ValueType::kObject), 12ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
                         *time_iter));
  time_iter = t.begin();
  ++key_string[1];

  // Stack 4
  InitializeCollection(key_string, &val_string, &time_iter, &docdb_dump);
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
    Value(PrimitiveValue(ValueType::kObject), 15ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
                         *time_iter));
  ++time_iter;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
    Value(PrimitiveValue(ValueType::kObject), 6ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
                         *time_iter));
  ++time_iter;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
    Value(PrimitiveValue(ValueType::kObject), 18ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
                         *time_iter));
  ++time_iter;
  InitializeCollection(key_string, &val_string, &time_iter, &docdb_dump);
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
    Value(PrimitiveValue(ValueType::kObject), 18ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
                         *time_iter));
  ++time_iter;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
    Value(PrimitiveValue(ValueType::kObject), 9ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
                         *time_iter));
  ++time_iter;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
    Value(PrimitiveValue(ValueType::kObject), Value::kMaxTtl,
          Value::kInvalidUserTimestamp, Value::kTtlFlag), *time_iter));

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
  SubDocKey(DocKey([], ["k0"]), [HT{ physical: 7000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 6000 }]) -> {}; merge flags: 1; ttl: 0.021s
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 4000 }]) -> {}
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 3000 }]) -> DEL
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 1000 }]) -> {}
SubDocKey(DocKey([], ["k0"]), ["sk0"; HT{ physical: 4000 w: 1 }]) -> "v6"
SubDocKey(DocKey([], ["k0"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "v0"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 5000 }]) -> "v9"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 4000 w: 2 }]) -> "v7"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 2000 }]) -> "v3"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 1000 w: 2 }]) -> "v1"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 5000 w: 1 }]) -> "v:"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 4000 w: 3 }]) -> "v8"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "v4"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 1000 w: 3 }]) -> "v2"
SubDocKey(DocKey([], ["k0"]), ["sk3"; HT{ physical: 5000 w: 2 }]) -> "v;"
SubDocKey(DocKey([], ["k0"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "v5"
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 5000 }]) -> DEL
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 4000 }]) -> {}; merge flags: 1; ttl: 0.015s
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 3000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 1000 }]) -> {}
SubDocKey(DocKey([], ["k1"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "v<"
SubDocKey(DocKey([], ["k1"]), ["sk1"; HT{ physical: 2000 }]) -> "v?"
SubDocKey(DocKey([], ["k1"]), ["sk1"; HT{ physical: 1000 w: 2 }]) -> "v="
SubDocKey(DocKey([], ["k1"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "v@"
SubDocKey(DocKey([], ["k1"]), ["sk2"; HT{ physical: 1000 w: 3 }]) -> "v>"
SubDocKey(DocKey([], ["k1"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "vA"
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 9000 }]) -> {}; merge flags: 1; ttl: 0.012s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 8000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 7000 }]) -> {}; merge flags: 1; ttl: 0.021s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 5000 }]) -> {}
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 4000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 3000 }]) -> {}; merge flags: 1; ttl: 0.015s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 1000 }]) -> {}
SubDocKey(DocKey([], ["k2"]), ["sk0"; HT{ physical: 5000 w: 1 }]) -> "vH"
SubDocKey(DocKey([], ["k2"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "vB"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 6000 }]) -> "vK"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 5000 w: 2 }]) -> "vI"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 2000 }]) -> "vE"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 1000 w: 2 }]) -> "vC"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 6000 w: 1 }]) -> "vL"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 5000 w: 3 }]) -> "vJ"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "vF"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 1000 w: 3 }]) -> "vD"
SubDocKey(DocKey([], ["k2"]), ["sk3"; HT{ physical: 6000 w: 2 }]) -> "vM"
SubDocKey(DocKey([], ["k2"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "vG"
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 10000 }]) -> {}; merge flags: 1
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 9000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 8000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 6000 }]) -> {}
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 5000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 4000 }]) -> {}; merge flags: 1; ttl: 0.006s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 3000 }]) -> {}; merge flags: 1; ttl: 0.015s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 1000 }]) -> {}
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 6000 w: 1 }]) -> "vT"
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "vN"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 7000 }]) -> "vW"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 6000 w: 2 }]) -> "vU"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 2000 }]) -> "vQ"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 1000 w: 2 }]) -> "vO"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 7000 w: 1 }]) -> "vX"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 6000 w: 3 }]) -> "vV"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "vR"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 1000 w: 3 }]) -> "vP"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 7000 w: 2 }]) -> "vY"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "vS"
      )#");
  FullyCompactHistoryBefore(t[0]);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 7000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 6000 }]) -> {}; merge flags: 1; ttl: 0.021s
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 4000 }]) -> {}
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 3000 }]) -> DEL
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 1000 }]) -> {}
SubDocKey(DocKey([], ["k0"]), ["sk0"; HT{ physical: 4000 w: 1 }]) -> "v6"
SubDocKey(DocKey([], ["k0"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "v0"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 5000 }]) -> "v9"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 4000 w: 2 }]) -> "v7"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 2000 }]) -> "v3"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 1000 w: 2 }]) -> "v1"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 5000 w: 1 }]) -> "v:"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 4000 w: 3 }]) -> "v8"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "v4"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 1000 w: 3 }]) -> "v2"
SubDocKey(DocKey([], ["k0"]), ["sk3"; HT{ physical: 5000 w: 2 }]) -> "v;"
SubDocKey(DocKey([], ["k0"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "v5"
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 5000 }]) -> DEL
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 4000 }]) -> {}; merge flags: 1; ttl: 0.015s
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 3000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 1000 }]) -> {}
SubDocKey(DocKey([], ["k1"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "v<"
SubDocKey(DocKey([], ["k1"]), ["sk1"; HT{ physical: 2000 }]) -> "v?"
SubDocKey(DocKey([], ["k1"]), ["sk1"; HT{ physical: 1000 w: 2 }]) -> "v="
SubDocKey(DocKey([], ["k1"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "v@"
SubDocKey(DocKey([], ["k1"]), ["sk2"; HT{ physical: 1000 w: 3 }]) -> "v>"
SubDocKey(DocKey([], ["k1"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "vA"
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 9000 }]) -> {}; merge flags: 1; ttl: 0.012s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 8000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 7000 }]) -> {}; merge flags: 1; ttl: 0.021s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 5000 }]) -> {}
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 4000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 3000 }]) -> {}; merge flags: 1; ttl: 0.015s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 1000 }]) -> {}
SubDocKey(DocKey([], ["k2"]), ["sk0"; HT{ physical: 5000 w: 1 }]) -> "vH"
SubDocKey(DocKey([], ["k2"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "vB"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 6000 }]) -> "vK"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 5000 w: 2 }]) -> "vI"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 2000 }]) -> "vE"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 1000 w: 2 }]) -> "vC"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 6000 w: 1 }]) -> "vL"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 5000 w: 3 }]) -> "vJ"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "vF"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 1000 w: 3 }]) -> "vD"
SubDocKey(DocKey([], ["k2"]), ["sk3"; HT{ physical: 6000 w: 2 }]) -> "vM"
SubDocKey(DocKey([], ["k2"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "vG"
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 10000 }]) -> {}; merge flags: 1
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 9000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 8000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 6000 }]) -> {}
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 5000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 4000 }]) -> {}; merge flags: 1; ttl: 0.006s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 3000 }]) -> {}; merge flags: 1; ttl: 0.015s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 1000 }]) -> {}
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 6000 w: 1 }]) -> "vT"
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "vN"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 7000 }]) -> "vW"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 6000 w: 2 }]) -> "vU"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 2000 }]) -> "vQ"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 1000 w: 2 }]) -> "vO"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 7000 w: 1 }]) -> "vX"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 6000 w: 3 }]) -> "vV"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "vR"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 1000 w: 3 }]) -> "vP"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 7000 w: 2 }]) -> "vY"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "vS"
      )#");
  FullyCompactHistoryBefore(t[1]);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 7000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 6000 }]) -> {}; merge flags: 1; ttl: 0.021s
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 4000 }]) -> {}
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 3000 }]) -> DEL
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 1000 }]) -> {}
SubDocKey(DocKey([], ["k0"]), ["sk0"; HT{ physical: 4000 w: 1 }]) -> "v6"
SubDocKey(DocKey([], ["k0"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "v0"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 5000 }]) -> "v9"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 4000 w: 2 }]) -> "v7"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 2000 }]) -> "v3"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 5000 w: 1 }]) -> "v:"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 4000 w: 3 }]) -> "v8"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "v4"
SubDocKey(DocKey([], ["k0"]), ["sk3"; HT{ physical: 5000 w: 2 }]) -> "v;"
SubDocKey(DocKey([], ["k0"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "v5"
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 5000 }]) -> DEL
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 4000 }]) -> {}; merge flags: 1; ttl: 0.015s
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 3000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 1000 }]) -> {}
SubDocKey(DocKey([], ["k1"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "v<"
SubDocKey(DocKey([], ["k1"]), ["sk1"; HT{ physical: 2000 }]) -> "v?"
SubDocKey(DocKey([], ["k1"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "v@"
SubDocKey(DocKey([], ["k1"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "vA"
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 9000 }]) -> {}; merge flags: 1; ttl: 0.012s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 8000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 7000 }]) -> {}; merge flags: 1; ttl: 0.021s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 5000 }]) -> {}
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 4000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 3000 }]) -> {}; merge flags: 1; ttl: 0.015s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 1000 }]) -> {}
SubDocKey(DocKey([], ["k2"]), ["sk0"; HT{ physical: 5000 w: 1 }]) -> "vH"
SubDocKey(DocKey([], ["k2"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "vB"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 6000 }]) -> "vK"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 5000 w: 2 }]) -> "vI"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 2000 }]) -> "vE"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 6000 w: 1 }]) -> "vL"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 5000 w: 3 }]) -> "vJ"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "vF"
SubDocKey(DocKey([], ["k2"]), ["sk3"; HT{ physical: 6000 w: 2 }]) -> "vM"
SubDocKey(DocKey([], ["k2"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "vG"
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 10000 }]) -> {}; merge flags: 1
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 9000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 8000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 6000 }]) -> {}
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 5000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 4000 }]) -> {}; merge flags: 1; ttl: 0.006s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 3000 }]) -> {}; merge flags: 1; ttl: 0.015s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 1000 }]) -> {}
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 6000 w: 1 }]) -> "vT"
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "vN"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 7000 }]) -> "vW"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 6000 w: 2 }]) -> "vU"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 2000 }]) -> "vQ"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 7000 w: 1 }]) -> "vX"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 6000 w: 3 }]) -> "vV"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "vR"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 7000 w: 2 }]) -> "vY"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "vS"
      )#");
  FullyCompactHistoryBefore(t[2]);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 7000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 6000 }]) -> {}; merge flags: 1; ttl: 0.021s
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 4000 }]) -> {}
SubDocKey(DocKey([], ["k0"]), ["sk0"; HT{ physical: 4000 w: 1 }]) -> "v6"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 5000 }]) -> "v9"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 4000 w: 2 }]) -> "v7"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 5000 w: 1 }]) -> "v:"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 4000 w: 3 }]) -> "v8"
SubDocKey(DocKey([], ["k0"]), ["sk3"; HT{ physical: 5000 w: 2 }]) -> "v;"
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 5000 }]) -> DEL
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 4000 }]) -> {}; merge flags: 1; ttl: 0.015s
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 1000 }]) -> {}; ttl: 0.020s
SubDocKey(DocKey([], ["k1"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "v<"
SubDocKey(DocKey([], ["k1"]), ["sk1"; HT{ physical: 2000 }]) -> "v?"
SubDocKey(DocKey([], ["k1"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "v@"
SubDocKey(DocKey([], ["k1"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "vA"
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 9000 }]) -> {}; merge flags: 1; ttl: 0.012s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 8000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 7000 }]) -> {}; merge flags: 1; ttl: 0.021s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 5000 }]) -> {}
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 4000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 1000 }]) -> {}; ttl: 0.017s
SubDocKey(DocKey([], ["k2"]), ["sk0"; HT{ physical: 5000 w: 1 }]) -> "vH"
SubDocKey(DocKey([], ["k2"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "vB"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 6000 }]) -> "vK"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 5000 w: 2 }]) -> "vI"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 2000 }]) -> "vE"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 6000 w: 1 }]) -> "vL"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 5000 w: 3 }]) -> "vJ"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "vF"
SubDocKey(DocKey([], ["k2"]), ["sk3"; HT{ physical: 6000 w: 2 }]) -> "vM"
SubDocKey(DocKey([], ["k2"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "vG"
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 10000 }]) -> {}; merge flags: 1
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 9000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 8000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 6000 }]) -> {}
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 5000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 4000 }]) -> {}; merge flags: 1; ttl: 0.006s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 1000 }]) -> {}; ttl: 0.017s
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 6000 w: 1 }]) -> "vT"
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "vN"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 7000 }]) -> "vW"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 6000 w: 2 }]) -> "vU"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 2000 }]) -> "vQ"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 7000 w: 1 }]) -> "vX"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 6000 w: 3 }]) -> "vV"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "vR"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 7000 w: 2 }]) -> "vY"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "vS"
      )#");
  FullyCompactHistoryBefore(t[3]);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 7000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 6000 }]) -> {}; merge flags: 1; ttl: 0.021s
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 4000 }]) -> {}
SubDocKey(DocKey([], ["k0"]), ["sk0"; HT{ physical: 4000 w: 1 }]) -> "v6"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 5000 }]) -> "v9"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 4000 w: 2 }]) -> "v7"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 5000 w: 1 }]) -> "v:"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 4000 w: 3 }]) -> "v8"
SubDocKey(DocKey([], ["k0"]), ["sk3"; HT{ physical: 5000 w: 2 }]) -> "v;"
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 5000 }]) -> DEL
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 1000 }]) -> {}; ttl: 0.018s
SubDocKey(DocKey([], ["k1"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "v<"
SubDocKey(DocKey([], ["k1"]), ["sk1"; HT{ physical: 2000 }]) -> "v?"
SubDocKey(DocKey([], ["k1"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "v@"
SubDocKey(DocKey([], ["k1"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "vA"
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 9000 }]) -> {}; merge flags: 1; ttl: 0.012s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 8000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 7000 }]) -> {}; merge flags: 1; ttl: 0.021s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 5000 }]) -> {}
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 1000 }]) -> {}; ttl: 0.021s
SubDocKey(DocKey([], ["k2"]), ["sk0"; HT{ physical: 5000 w: 1 }]) -> "vH"
SubDocKey(DocKey([], ["k2"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "vB"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 6000 }]) -> "vK"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 5000 w: 2 }]) -> "vI"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 2000 }]) -> "vE"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 6000 w: 1 }]) -> "vL"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 5000 w: 3 }]) -> "vJ"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "vF"
SubDocKey(DocKey([], ["k2"]), ["sk3"; HT{ physical: 6000 w: 2 }]) -> "vM"
SubDocKey(DocKey([], ["k2"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "vG"
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 10000 }]) -> {}; merge flags: 1
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 9000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 8000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 6000 }]) -> {}
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 5000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 1000 }]) -> {}; ttl: 0.009s
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 6000 w: 1 }]) -> "vT"
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "vN"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 7000 }]) -> "vW"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 6000 w: 2 }]) -> "vU"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 2000 }]) -> "vQ"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 7000 w: 1 }]) -> "vX"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 6000 w: 3 }]) -> "vV"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "vR"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 7000 w: 2 }]) -> "vY"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "vS"
      )#");
  FullyCompactHistoryBefore(t[4]);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 7000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 6000 }]) -> {}; merge flags: 1; ttl: 0.021s
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 4000 }]) -> {}
SubDocKey(DocKey([], ["k0"]), ["sk0"; HT{ physical: 4000 w: 1 }]) -> "v6"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 5000 }]) -> "v9"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 5000 w: 1 }]) -> "v:"
SubDocKey(DocKey([], ["k0"]), ["sk3"; HT{ physical: 5000 w: 2 }]) -> "v;"
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 9000 }]) -> {}; merge flags: 1; ttl: 0.012s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 8000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 7000 }]) -> {}; merge flags: 1; ttl: 0.021s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 5000 }]) -> {}
SubDocKey(DocKey([], ["k2"]), ["sk0"; HT{ physical: 5000 w: 1 }]) -> "vH"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 6000 }]) -> "vK"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 5000 w: 2 }]) -> "vI"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 6000 w: 1 }]) -> "vL"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 5000 w: 3 }]) -> "vJ"
SubDocKey(DocKey([], ["k2"]), ["sk3"; HT{ physical: 6000 w: 2 }]) -> "vM"
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 10000 }]) -> {}; merge flags: 1
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 9000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 8000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 6000 }]) -> {}
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 1000 }]) -> {}; ttl: 0.022s
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 6000 w: 1 }]) -> "vT"
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 1000 w: 1 }]) -> "vN"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 7000 }]) -> "vW"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 6000 w: 2 }]) -> "vU"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 2000 }]) -> "vQ"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 7000 w: 1 }]) -> "vX"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 6000 w: 3 }]) -> "vV"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 2000 w: 1 }]) -> "vR"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 7000 w: 2 }]) -> "vY"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 2000 w: 2 }]) -> "vS"
      )#");
  FullyCompactHistoryBefore(t[5]);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 7000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 4000 }]) -> {}; ttl: 0.023s
SubDocKey(DocKey([], ["k0"]), ["sk0"; HT{ physical: 4000 w: 1 }]) -> "v6"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 5000 }]) -> "v9"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 5000 w: 1 }]) -> "v:"
SubDocKey(DocKey([], ["k0"]), ["sk3"; HT{ physical: 5000 w: 2 }]) -> "v;"
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 9000 }]) -> {}; merge flags: 1; ttl: 0.012s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 8000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 7000 }]) -> {}; merge flags: 1; ttl: 0.021s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 5000 }]) -> {}
SubDocKey(DocKey([], ["k2"]), ["sk0"; HT{ physical: 5000 w: 1 }]) -> "vH"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 6000 }]) -> "vK"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 6000 w: 1 }]) -> "vL"
SubDocKey(DocKey([], ["k2"]), ["sk3"; HT{ physical: 6000 w: 2 }]) -> "vM"
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 10000 }]) -> {}; merge flags: 1
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 9000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 8000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 6000 }]) -> {}
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 6000 w: 1 }]) -> "vT"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 7000 }]) -> "vW"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 6000 w: 2 }]) -> "vU"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 7000 w: 1 }]) -> "vX"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 6000 w: 3 }]) -> "vV"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 7000 w: 2 }]) -> "vY"
      )#");
  FullyCompactHistoryBefore(t[6]);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 4000 }]) -> {}; ttl: 0.012s
SubDocKey(DocKey([], ["k0"]), ["sk0"; HT{ physical: 4000 w: 1 }]) -> "v6"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 5000 }]) -> "v9"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 5000 w: 1 }]) -> "v:"
SubDocKey(DocKey([], ["k0"]), ["sk3"; HT{ physical: 5000 w: 2 }]) -> "v;"
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 9000 }]) -> {}; merge flags: 1; ttl: 0.012s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 8000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 5000 }]) -> {}; ttl: 0.023s
SubDocKey(DocKey([], ["k2"]), ["sk0"; HT{ physical: 5000 w: 1 }]) -> "vH"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 6000 }]) -> "vK"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 6000 w: 1 }]) -> "vL"
SubDocKey(DocKey([], ["k2"]), ["sk3"; HT{ physical: 6000 w: 2 }]) -> "vM"
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 10000 }]) -> {}; merge flags: 1
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 9000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 8000 }]) -> {}; merge flags: 1; ttl: 0.018s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 6000 }]) -> {}
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 6000 w: 1 }]) -> "vT"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 7000 }]) -> "vW"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 7000 w: 1 }]) -> "vX"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 7000 w: 2 }]) -> "vY"
      )#");
  FullyCompactHistoryBefore(t[7]);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 4000 }]) -> {}; ttl: 0.012s
SubDocKey(DocKey([], ["k0"]), ["sk0"; HT{ physical: 4000 w: 1 }]) -> "v6"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 5000 }]) -> "v9"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 5000 w: 1 }]) -> "v:"
SubDocKey(DocKey([], ["k0"]), ["sk3"; HT{ physical: 5000 w: 2 }]) -> "v;"
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 9000 }]) -> {}; merge flags: 1; ttl: 0.012s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 5000 }]) -> {}; ttl: 0.012s
SubDocKey(DocKey([], ["k2"]), ["sk0"; HT{ physical: 5000 w: 1 }]) -> "vH"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 6000 }]) -> "vK"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 6000 w: 1 }]) -> "vL"
SubDocKey(DocKey([], ["k2"]), ["sk3"; HT{ physical: 6000 w: 2 }]) -> "vM"
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 10000 }]) -> {}; merge flags: 1
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 9000 }]) -> {}; merge flags: 1; ttl: 0.009s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 6000 }]) -> {}; ttl: 0.020s
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 6000 w: 1 }]) -> "vT"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 7000 }]) -> "vW"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 7000 w: 1 }]) -> "vX"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 7000 w: 2 }]) -> "vY"
      )#");
  FullyCompactHistoryBefore(t[8]);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 4000 }]) -> {}; ttl: 0.012s
SubDocKey(DocKey([], ["k0"]), ["sk0"; HT{ physical: 4000 w: 1 }]) -> "v6"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 5000 }]) -> "v9"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 5000 w: 1 }]) -> "v:"
SubDocKey(DocKey([], ["k0"]), ["sk3"; HT{ physical: 5000 w: 2 }]) -> "v;"
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 5000 }]) -> {}; ttl: 0.016s
SubDocKey(DocKey([], ["k2"]), ["sk0"; HT{ physical: 5000 w: 1 }]) -> "vH"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 6000 }]) -> "vK"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 6000 w: 1 }]) -> "vL"
SubDocKey(DocKey([], ["k2"]), ["sk3"; HT{ physical: 6000 w: 2 }]) -> "vM"
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 10000 }]) -> {}; merge flags: 1
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 6000 }]) -> {}; ttl: 0.012s
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 6000 w: 1 }]) -> "vT"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 7000 }]) -> "vW"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 7000 w: 1 }]) -> "vX"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 7000 w: 2 }]) -> "vY"
      )#");
  FullyCompactHistoryBefore(t[9]);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 4000 }]) -> {}; ttl: 0.012s
SubDocKey(DocKey([], ["k0"]), ["sk0"; HT{ physical: 4000 w: 1 }]) -> "v6"
SubDocKey(DocKey([], ["k0"]), ["sk1"; HT{ physical: 5000 }]) -> "v9"
SubDocKey(DocKey([], ["k0"]), ["sk2"; HT{ physical: 5000 w: 1 }]) -> "v:"
SubDocKey(DocKey([], ["k0"]), ["sk3"; HT{ physical: 5000 w: 2 }]) -> "v;"
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 5000 }]) -> {}; ttl: 0.016s
SubDocKey(DocKey([], ["k2"]), ["sk0"; HT{ physical: 5000 w: 1 }]) -> "vH"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 6000 }]) -> "vK"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 6000 w: 1 }]) -> "vL"
SubDocKey(DocKey([], ["k2"]), ["sk3"; HT{ physical: 6000 w: 2 }]) -> "vM"
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 6000 }]) -> {}
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 6000 w: 1 }]) -> "vT"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 7000 }]) -> "vW"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 7000 w: 1 }]) -> "vX"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 7000 w: 2 }]) -> "vY"
      )#");
  FullyCompactHistoryBefore(t[16]);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 5000 }]) -> {}; ttl: 0.016s
SubDocKey(DocKey([], ["k2"]), ["sk0"; HT{ physical: 5000 w: 1 }]) -> "vH"
SubDocKey(DocKey([], ["k2"]), ["sk1"; HT{ physical: 6000 }]) -> "vK"
SubDocKey(DocKey([], ["k2"]), ["sk2"; HT{ physical: 6000 w: 1 }]) -> "vL"
SubDocKey(DocKey([], ["k2"]), ["sk3"; HT{ physical: 6000 w: 2 }]) -> "vM"
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 6000 }]) -> {}
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 6000 w: 1 }]) -> "vT"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 7000 }]) -> "vW"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 7000 w: 1 }]) -> "vX"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 7000 w: 2 }]) -> "vY"
      )#");
  FullyCompactHistoryBefore(t[21]);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 6000 }]) -> {}
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 6000 w: 1 }]) -> "vT"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 7000 }]) -> "vW"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 7000 w: 1 }]) -> "vX"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 7000 w: 2 }]) -> "vY"
      )#");
  FullyCompactHistoryBefore(t[34]);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 6000 }]) -> {}
SubDocKey(DocKey([], ["k3"]), ["sk0"; HT{ physical: 6000 w: 1 }]) -> "vT"
SubDocKey(DocKey([], ["k3"]), ["sk1"; HT{ physical: 7000 }]) -> "vW"
SubDocKey(DocKey([], ["k3"]), ["sk2"; HT{ physical: 7000 w: 1 }]) -> "vX"
SubDocKey(DocKey([], ["k3"]), ["sk3"; HT{ physical: 7000 w: 2 }]) -> "vY"
      )#");
}

// Basic compaction testing for TTL in Redis.
TEST_P(DocDBTestWrapper, RedisTTLCompactionTest) {
  const MonoDelta one_ms = 1ms;
  string key_string = "k0";
  string val_string = "v0";
  int n_times = 20;
  vector<HybridTime> t(n_times);
  t[0] = 1000_usec_ht;
  for (int i = 1; i < n_times; ++i) {
    t[i] = server::HybridClock::AddPhysicalTimeToHybridTime(t[i-1], one_ms);
  }
  // Compact at t10
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(), // k0
                         Value(PrimitiveValue(val_string), 4ms), t[2]));
  val_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
                         Value(PrimitiveValue(val_string), 3ms), t[0]));
  val_string[1]++;
  key_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(), // k1
                         Value(PrimitiveValue(val_string), 8ms), t[3]));
  val_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
                         Value(PrimitiveValue(val_string), 1ms), t[5]));
  val_string[1]++;
  key_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(), // k2
                         Value(PrimitiveValue(val_string), 3ms), t[5]));
  val_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
                         Value(PrimitiveValue(val_string), 5ms), t[7]));
  val_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
                         Value(PrimitiveValue(val_string), Value::kMaxTtl), t[11]));
  key_string[1]++;
  val_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(), // k3
                         Value(PrimitiveValue(val_string), 4ms), t[1]));
  val_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
                         Value(PrimitiveValue(val_string), Value::kMaxTtl), t[4]));
  val_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
                         Value(PrimitiveValue(val_string), 1ms), t[13]));
  val_string[1]++;
  key_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(), // k4
                         Value(PrimitiveValue::kTombstone), t[12]));
  key_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(), // k5
                         Value(PrimitiveValue(val_string), 9ms), t[8]));
  val_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
                         Value(PrimitiveValue::kTombstone), t[9]));
  key_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(), // k6
                         Value(PrimitiveValue(val_string), 9ms), t[8]));
  val_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
                         Value(PrimitiveValue::kTombstone), t[6]));
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 3000 }]) -> "v0"; ttl: 0.004s
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 1000 }]) -> "v1"; ttl: 0.003s
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 6000 }]) -> "v3"; ttl: 0.001s
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 4000 }]) -> "v2"; ttl: 0.008s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 12000 }]) -> "v6"
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 8000 }]) -> "v5"; ttl: 0.005s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 6000 }]) -> "v4"; ttl: 0.003s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 14000 }]) -> "v9"; ttl: 0.001s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 5000 }]) -> "v8"
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 2000 }]) -> "v7"; ttl: 0.004s
SubDocKey(DocKey([], ["k4"]), [HT{ physical: 13000 }]) -> DEL
SubDocKey(DocKey([], ["k5"]), [HT{ physical: 10000 }]) -> DEL
SubDocKey(DocKey([], ["k5"]), [HT{ physical: 9000 }]) -> "v:"; ttl: 0.009s
SubDocKey(DocKey([], ["k6"]), [HT{ physical: 9000 }]) -> "v;"; ttl: 0.009s
SubDocKey(DocKey([], ["k6"]), [HT{ physical: 7000 }]) -> DEL
      )#");
  FullyCompactHistoryBefore(t[10]);

  // Major compaction
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 12000 }]) -> "v6"
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 8000 }]) -> "v5"; ttl: 0.005s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 14000 }]) -> "v9"; ttl: 0.001s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 5000 }]) -> "v8"
SubDocKey(DocKey([], ["k4"]), [HT{ physical: 13000 }]) -> DEL
SubDocKey(DocKey([], ["k6"]), [HT{ physical: 9000 }]) -> "v;"; ttl: 0.009s
      )#");

  FullyCompactHistoryBefore(t[14]);

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 12000 }]) -> "v6"
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 14000 }]) -> "v9"; ttl: 0.001s
SubDocKey(DocKey([], ["k6"]), [HT{ physical: 9000 }]) -> "v;"; ttl: 0.009s
      )#");

  FullyCompactHistoryBefore(t[19]);

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 12000 }]) -> "v6"
      )#");

  key_string = "k0";
  val_string = "v0";
  // Checking TTL rows now
  ASSERT_OK(SetPrimitive(
      DocKey(PrimitiveValues(key_string)).Encode(), // k0
      Value(PrimitiveValue(ValueType::kString), 6ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
      t[5]));
  ASSERT_OK(SetPrimitive(
      DocKey(PrimitiveValues(key_string)).Encode(),
      Value(PrimitiveValue(ValueType::kString), 4ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
      t[2]));
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
                         Value(PrimitiveValue(val_string), 3ms), t[0]));
  val_string[1]++;
  key_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(), // k1
                         Value(PrimitiveValue(val_string), 8ms), t[3]));
  val_string[1]++;
  ASSERT_OK(SetPrimitive(
      DocKey(PrimitiveValues(key_string)).Encode(),
      Value(PrimitiveValue(ValueType::kString), 3ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
      t[5]));
  key_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(), // k2
                         Value(PrimitiveValue(val_string), 3ms), t[5]));
  val_string[1]++;
  ASSERT_OK(SetPrimitive(
      DocKey(PrimitiveValues(key_string)).Encode(),
      Value(PrimitiveValue(ValueType::kString), 5ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
      t[7]));
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
      Value(PrimitiveValue(ValueType::kString), Value::kMaxTtl, Value::kInvalidUserTimestamp,
      Value::kTtlFlag), t[11]));
  key_string[1]++;
  ASSERT_OK(SetPrimitive( // k3
  DocKey(PrimitiveValues(key_string)).Encode(),
                         Value(PrimitiveValue(val_string), 4ms), t[1]));
  val_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
      Value(PrimitiveValue(ValueType::kString), Value::kMaxTtl, Value::kInvalidUserTimestamp,
      Value::kTtlFlag), t[4]));
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
                         Value(PrimitiveValue(val_string), 1ms), t[13]));
  val_string[1]++;
  key_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(), // k4
                         Value(PrimitiveValue::kTombstone), t[12]));
  key_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(), // k5
                         Value(PrimitiveValue(val_string), 9ms), t[8]));
  val_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
                         Value(PrimitiveValue::kTombstone), t[9]));
  key_string[1]++;
  ASSERT_OK(SetPrimitive( // k6
      DocKey(PrimitiveValues(key_string)).Encode(),
      Value(PrimitiveValue(ValueType::kString), 4ms, Value::kInvalidUserTimestamp, Value::kTtlFlag),
      t[10]));

  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
                         Value(PrimitiveValue(val_string), 9ms), t[8]));
  val_string[1]++;
  ASSERT_OK(SetPrimitive(DocKey(PrimitiveValues(key_string)).Encode(),
                         Value(PrimitiveValue::kTombstone), t[6]));

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 6000 }]) -> ""; merge flags: 1; ttl: 0.006s
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 3000 }]) -> ""; merge flags: 1; ttl: 0.004s
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 1000 }]) -> "v0"; ttl: 0.003s
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 6000 }]) -> ""; merge flags: 1; ttl: 0.003s
SubDocKey(DocKey([], ["k1"]), [HT{ physical: 4000 }]) -> "v1"; ttl: 0.008s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 12000 }]) -> ""; merge flags: 1
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 8000 }]) -> ""; merge flags: 1; ttl: 0.005s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 6000 }]) -> "v2"; ttl: 0.003s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 14000 }]) -> "v4"; ttl: 0.001s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 5000 }]) -> ""; merge flags: 1
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 2000 }]) -> "v3"; ttl: 0.004s
SubDocKey(DocKey([], ["k4"]), [HT{ physical: 13000 }]) -> DEL
SubDocKey(DocKey([], ["k5"]), [HT{ physical: 10000 }]) -> DEL
SubDocKey(DocKey([], ["k5"]), [HT{ physical: 9000 }]) -> "v5"; ttl: 0.009s
SubDocKey(DocKey([], ["k6"]), [HT{ physical: 11000 }]) -> ""; merge flags: 1; ttl: 0.004s
SubDocKey(DocKey([], ["k6"]), [HT{ physical: 9000 }]) -> "v6"; ttl: 0.009s
SubDocKey(DocKey([], ["k6"]), [HT{ physical: 7000 }]) -> DEL
      )#");
  FullyCompactHistoryBefore(t[9]);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k0"]), [HT{ physical: 1000 }]) -> "v0"; ttl: 0.011s
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 12000 }]) -> ""; merge flags: 1
SubDocKey(DocKey([], ["k2"]), [HT{ physical: 6000 }]) -> "v2"; ttl: 0.007s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 14000 }]) -> "v4"; ttl: 0.001s
SubDocKey(DocKey([], ["k3"]), [HT{ physical: 2000 }]) -> "v3"
SubDocKey(DocKey([], ["k4"]), [HT{ physical: 13000 }]) -> DEL
SubDocKey(DocKey([], ["k6"]), [HT{ physical: 11000 }]) -> ""; merge flags: 1; ttl: 0.004s
SubDocKey(DocKey([], ["k6"]), [HT{ physical: 9000 }]) -> "v6"; ttl: 0.009s
      )#");
}

TEST_P(DocDBTestWrapper, TTLCompactionTest) {
  const DocKey doc_key(PrimitiveValues("k1"));
  const MonoDelta one_ms = 1ms;
  const HybridTime t0 = 1000_usec_ht;
  HybridTime t1 = server::HybridClock::AddPhysicalTimeToHybridTime(t0, one_ms);
  HybridTime t2 = server::HybridClock::AddPhysicalTimeToHybridTime(t1, one_ms);
  HybridTime t3 = server::HybridClock::AddPhysicalTimeToHybridTime(t2, one_ms);
  HybridTime t4 = server::HybridClock::AddPhysicalTimeToHybridTime(t3, one_ms);
  KeyBytes encoded_doc_key(doc_key.Encode());
  // First row.
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key,
      PrimitiveValue::SystemColumnId(SystemColumnIds::kLivenessColumn)),
      Value(PrimitiveValue(), 1ms), t0));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(ColumnId(0))),
      Value(PrimitiveValue("v1"), 2ms), t0));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(ColumnId(1))),
      Value(PrimitiveValue("v2"), 3ms), t0));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(ColumnId(2))),
      Value(PrimitiveValue("v3"), Value::kMaxTtl), t0));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(ColumnId(3))),
      Value(PrimitiveValue("v4"), Value::kMaxTtl), t0));
  // Second row.
  const DocKey doc_key_row2(PrimitiveValues("k2"));
  KeyBytes encoded_doc_key_row2(doc_key_row2.Encode());
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key_row2,
      PrimitiveValue::SystemColumnId(SystemColumnIds::kLivenessColumn)),
      Value(PrimitiveValue(), 3ms), t0));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key_row2, PrimitiveValue(ColumnId(0))),
      Value(PrimitiveValue("v1"), 2ms), t0));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key_row2, PrimitiveValue(ColumnId(1))),
      Value(PrimitiveValue("v2"), 1ms), t0));
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k1"]), [SystemColumnId(0); HT{ physical: 1000 }]) -> null; ttl: 0.001s
SubDocKey(DocKey([], ["k1"]), [ColumnId(0); HT{ physical: 1000 }]) -> "v1"; ttl: 0.002s
SubDocKey(DocKey([], ["k1"]), [ColumnId(1); HT{ physical: 1000 }]) -> "v2"; ttl: 0.003s
SubDocKey(DocKey([], ["k1"]), [ColumnId(2); HT{ physical: 1000 }]) -> "v3"
SubDocKey(DocKey([], ["k1"]), [ColumnId(3); HT{ physical: 1000 }]) -> "v4"
SubDocKey(DocKey([], ["k2"]), [SystemColumnId(0); HT{ physical: 1000 }]) -> null; ttl: 0.003s
SubDocKey(DocKey([], ["k2"]), [ColumnId(0); HT{ physical: 1000 }]) -> "v1"; ttl: 0.002s
SubDocKey(DocKey([], ["k2"]), [ColumnId(1); HT{ physical: 1000 }]) -> "v2"; ttl: 0.001s
      )#");

  FullyCompactHistoryBefore(t2);

  // Liveness column is gone for row1, v2 gone for row2.
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k1"]), [ColumnId(0); HT{ physical: 1000 }]) -> "v1"; ttl: 0.002s
SubDocKey(DocKey([], ["k1"]), [ColumnId(1); HT{ physical: 1000 }]) -> "v2"; ttl: 0.003s
SubDocKey(DocKey([], ["k1"]), [ColumnId(2); HT{ physical: 1000 }]) -> "v3"
SubDocKey(DocKey([], ["k1"]), [ColumnId(3); HT{ physical: 1000 }]) -> "v4"
SubDocKey(DocKey([], ["k2"]), [SystemColumnId(0); HT{ physical: 1000 }]) -> null; ttl: 0.003s
SubDocKey(DocKey([], ["k2"]), [ColumnId(0); HT{ physical: 1000 }]) -> "v1"; ttl: 0.002s
      )#");

  FullyCompactHistoryBefore(t3);

  // v1 is gone.
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k1"]), [ColumnId(1); HT{ physical: 1000 }]) -> "v2"; ttl: 0.003s
SubDocKey(DocKey([], ["k1"]), [ColumnId(2); HT{ physical: 1000 }]) -> "v3"
SubDocKey(DocKey([], ["k1"]), [ColumnId(3); HT{ physical: 1000 }]) -> "v4"
SubDocKey(DocKey([], ["k2"]), [SystemColumnId(0); HT{ physical: 1000 }]) -> null; ttl: 0.003s
      )#");

  FullyCompactHistoryBefore(t4);
  // v2 is gone for row 1, liveness column gone for row 2.
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k1"]), [ColumnId(2); HT{ physical: 1000 }]) -> "v3"
SubDocKey(DocKey([], ["k1"]), [ColumnId(3); HT{ physical: 1000 }]) -> "v4"
      )#");

  // Delete values.
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(ColumnId(2))),
      Value(PrimitiveValue::kTombstone, Value::kMaxTtl), t1));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue(ColumnId(3))),
      Value(PrimitiveValue::kTombstone, Value::kMaxTtl), t1));

  // Values are now marked with tombstones.
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k1"]), [ColumnId(2); HT{ physical: 2000 }]) -> DEL
SubDocKey(DocKey([], ["k1"]), [ColumnId(2); HT{ physical: 1000 }]) -> "v3"
SubDocKey(DocKey([], ["k1"]), [ColumnId(3); HT{ physical: 2000 }]) -> DEL
SubDocKey(DocKey([], ["k1"]), [ColumnId(3); HT{ physical: 1000 }]) -> "v4"
      )#");

  FullyCompactHistoryBefore(t0);
  // Nothing is removed.
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k1"]), [ColumnId(2); HT{ physical: 2000 }]) -> DEL
SubDocKey(DocKey([], ["k1"]), [ColumnId(2); HT{ physical: 1000 }]) -> "v3"
SubDocKey(DocKey([], ["k1"]), [ColumnId(3); HT{ physical: 2000 }]) -> DEL
SubDocKey(DocKey([], ["k1"]), [ColumnId(3); HT{ physical: 1000 }]) -> "v4"
      )#");

  FullyCompactHistoryBefore(t1);
  // Next compactions removes everything.
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
      )#");
}

TEST_P(DocDBTestWrapper, TableTTLCompactionTest) {
  const DocKey doc_key(PrimitiveValues("k1"));
  const HybridTime t1 = 1000_usec_ht;
  const HybridTime t2 = 2000_usec_ht;
  const HybridTime t3 = 3000_usec_ht;
  const HybridTime t4 = 4000_usec_ht;
  const HybridTime t5 = 5000_usec_ht;
  KeyBytes encoded_doc_key(doc_key.Encode());
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue("s1")),
      Value(PrimitiveValue("v1"), 1ms), t1));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue("s2")),
      Value(PrimitiveValue("v2"), Value::kMaxTtl), t1));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue("s3")),
      Value(PrimitiveValue("v3"), 0ms), t2));
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue("s4")),
      Value(PrimitiveValue("v4"), 3ms), t1));
  // Note: HT{ physical: 1000 } + 1ms = HT{ physical: 4097000 }
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["k1"]), ["s1"; HT{ physical: 1000 }]) -> "v1"; ttl: 0.001s
      SubDocKey(DocKey([], ["k1"]), ["s2"; HT{ physical: 1000 }]) -> "v2"
      SubDocKey(DocKey([], ["k1"]), ["s3"; HT{ physical: 2000 }]) -> "v3"; ttl: 0.000s
      SubDocKey(DocKey([], ["k1"]), ["s4"; HT{ physical: 1000 }]) -> "v4"; ttl: 0.003s
      )#");
  SetTableTTL(2);
  FullyCompactHistoryBefore(t3);

  // v1 compacted due to column level ttl.
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k1"]), ["s2"; HT{ physical: 1000 }]) -> "v2"
SubDocKey(DocKey([], ["k1"]), ["s3"; HT{ physical: 2000 }]) -> "v3"; ttl: 0.000s
SubDocKey(DocKey([], ["k1"]), ["s4"; HT{ physical: 1000 }]) -> "v4"; ttl: 0.003s
      )#");

  FullyCompactHistoryBefore(t4);
  // v2 compacted due to table level ttl.
  // init marker compacted due to table level ttl.
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k1"]), ["s3"; HT{ physical: 2000 }]) -> "v3"; ttl: 0.000s
SubDocKey(DocKey([], ["k1"]), ["s4"; HT{ physical: 1000 }]) -> "v4"; ttl: 0.003s
      )#");

  FullyCompactHistoryBefore(t5);
  // v4 compacted due to column level ttl.
  // v3 stays forever due to ttl being set to 0.
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k1"]), ["s3"; HT{ physical: 2000 }]) -> "v3"; ttl: 0.000s
      )#");
}

// Test table tombstones for colocated tables.
TEST_P(DocDBTestWrapper, TableTombstoneCompaction) {
  constexpr PgTableOid pgtable_id(0x4001);
  HybridTime t = 1000_usec_ht;

  // Simulate SQL:
  //   INSERT INTO t VALUES ("r1"), ("r2"), ("r3");
  for (int i = 1; i <= 3; ++i) {
    DocKey doc_key;
    std::string range_key_str = Format("r$0", i);

    doc_key.set_pgtable_id(pgtable_id);
    doc_key.ResizeRangeComponents(1);
    doc_key.SetRangeComponent(PrimitiveValue(range_key_str), 0 /* idx */);
    ASSERT_OK(SetPrimitive(
        DocPath(doc_key.Encode(), PrimitiveValue::SystemColumnId(SystemColumnIds::kLivenessColumn)),
        Value(PrimitiveValue()),
        t));
    t = server::HybridClock::AddPhysicalTimeToHybridTime(t, 1ms);
  }
  ASSERT_OK(FlushRocksDbAndWait());
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey(PgTableId=16385, [], ["r1"]), [SystemColumnId(0); HT{ physical: 1000 }]) -> null
SubDocKey(DocKey(PgTableId=16385, [], ["r2"]), [SystemColumnId(0); HT{ physical: 2000 }]) -> null
SubDocKey(DocKey(PgTableId=16385, [], ["r3"]), [SystemColumnId(0); HT{ physical: 3000 }]) -> null
      )#");

  // Simulate SQL (set table tombstone):
  //   TRUNCATE TABLE t;
  {
    DocKey doc_key(pgtable_id);
    ASSERT_OK(SetPrimitive(
        DocPath(doc_key.Encode()),
        Value(PrimitiveValue::kTombstone),
        t));
    t = server::HybridClock::AddPhysicalTimeToHybridTime(t, 1ms);
  }
  ASSERT_OK(FlushRocksDbAndWait());
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey(PgTableId=16385, [], []), [HT{ physical: 4000 }]) -> DEL
SubDocKey(DocKey(PgTableId=16385, [], ["r1"]), [SystemColumnId(0); HT{ physical: 1000 }]) -> null
SubDocKey(DocKey(PgTableId=16385, [], ["r2"]), [SystemColumnId(0); HT{ physical: 2000 }]) -> null
SubDocKey(DocKey(PgTableId=16385, [], ["r3"]), [SystemColumnId(0); HT{ physical: 3000 }]) -> null
      )#");

  // Simulate SQL:
  //  INSERT INTO t VALUES ("r1"), ("r2");
  for (int i = 1; i <= 2; ++i) {
    DocKey doc_key;
    std::string range_key_str = Format("r$0", i);

    doc_key.set_pgtable_id(pgtable_id);
    doc_key.ResizeRangeComponents(1);
    doc_key.SetRangeComponent(PrimitiveValue(range_key_str), 0 /* idx */);
    ASSERT_OK(SetPrimitive(
        DocPath(doc_key.Encode(), PrimitiveValue::SystemColumnId(SystemColumnIds::kLivenessColumn)),
        Value(PrimitiveValue()),
        t));
    t = server::HybridClock::AddPhysicalTimeToHybridTime(t, 1ms);
  }
  ASSERT_OK(FlushRocksDbAndWait());
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey(PgTableId=16385, [], []), [HT{ physical: 4000 }]) -> DEL
SubDocKey(DocKey(PgTableId=16385, [], ["r1"]), [SystemColumnId(0); HT{ physical: 5000 }]) -> null
SubDocKey(DocKey(PgTableId=16385, [], ["r1"]), [SystemColumnId(0); HT{ physical: 1000 }]) -> null
SubDocKey(DocKey(PgTableId=16385, [], ["r2"]), [SystemColumnId(0); HT{ physical: 6000 }]) -> null
SubDocKey(DocKey(PgTableId=16385, [], ["r2"]), [SystemColumnId(0); HT{ physical: 2000 }]) -> null
SubDocKey(DocKey(PgTableId=16385, [], ["r3"]), [SystemColumnId(0); HT{ physical: 3000 }]) -> null
      )#");

  // Simulate SQL:
  //  DELETE FROM t WHERE c = "r2";
  {
    DocKey doc_key;
    std::string range_key_str = Format("r$0", 2);

    doc_key.set_pgtable_id(pgtable_id);
    doc_key.ResizeRangeComponents(1);
    doc_key.SetRangeComponent(PrimitiveValue(range_key_str), 0 /* idx */);
    ASSERT_OK(SetPrimitive(
        DocPath(doc_key.Encode()),
        Value(PrimitiveValue::kTombstone),
        t));
    t = server::HybridClock::AddPhysicalTimeToHybridTime(t, 1ms);
  }
  ASSERT_OK(FlushRocksDbAndWait());
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey(PgTableId=16385, [], []), [HT{ physical: 4000 }]) -> DEL
SubDocKey(DocKey(PgTableId=16385, [], ["r1"]), [SystemColumnId(0); HT{ physical: 5000 }]) -> null
SubDocKey(DocKey(PgTableId=16385, [], ["r1"]), [SystemColumnId(0); HT{ physical: 1000 }]) -> null
SubDocKey(DocKey(PgTableId=16385, [], ["r2"]), [HT{ physical: 7000 }]) -> DEL
SubDocKey(DocKey(PgTableId=16385, [], ["r2"]), [SystemColumnId(0); HT{ physical: 6000 }]) -> null
SubDocKey(DocKey(PgTableId=16385, [], ["r2"]), [SystemColumnId(0); HT{ physical: 2000 }]) -> null
SubDocKey(DocKey(PgTableId=16385, [], ["r3"]), [SystemColumnId(0); HT{ physical: 3000 }]) -> null
      )#");

  // Major compact.
  FullyCompactHistoryBefore(10000_usec_ht);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey(PgTableId=16385, [], ["r1"]), [SystemColumnId(0); HT{ physical: 5000 }]) -> null
      )#");
}

TEST_P(DocDBTestWrapper, MinorCompactionNoDeletions) {
  ASSERT_OK(DisableCompactions());
  const DocKey doc_key(PrimitiveValues("k"));
  KeyBytes encoded_doc_key(doc_key.Encode());
  for (int i = 1; i <= 6; ++i) {
    auto value_str = Format("v$0", i);
    PrimitiveValue pv(value_str);
    ASSERT_OK(SetPrimitive(
        DocPath(encoded_doc_key), Value(pv), HybridTime::FromMicros(i * 1000)));
    ASSERT_OK(FlushRocksDbAndWait());
  }

  ASSERT_EQ(6, NumSSTableFiles());
  const char* kInitialDocDbStateStr = R"#(
SubDocKey(DocKey([], ["k"]), [HT{ physical: 6000 }]) -> "v6"  // file 6
SubDocKey(DocKey([], ["k"]), [HT{ physical: 5000 }]) -> "v5"  // file 5
SubDocKey(DocKey([], ["k"]), [HT{ physical: 4000 }]) -> "v4"  // file 4
SubDocKey(DocKey([], ["k"]), [HT{ physical: 3000 }]) -> "v3"  // file 3
SubDocKey(DocKey([], ["k"]), [HT{ physical: 2000 }]) -> "v2"  // file 2
SubDocKey(DocKey([], ["k"]), [HT{ physical: 1000 }]) -> "v1"  // file 1
      )#";

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(kInitialDocDbStateStr);
  MinorCompaction(5000_usec_ht, /* num_files_to_compact */ 2);

  ASSERT_EQ(5, NumSSTableFiles());
  // No changes in DocDB rows as we still need the entry at 5000_ms_ht.
  // Let's call the output file resulting from the last compaction "file 7".
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(kInitialDocDbStateStr);

  MinorCompaction(5000_usec_ht, /* num_files_to_compact */ 2);
  ASSERT_EQ(4, NumSSTableFiles());
  // Removed the entry at 4000_ms_ht as it was overwritten at time 5000. Earlier entries are in
  // other files that haven't been compacted yet.
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k"]), [HT{ physical: 6000 }]) -> "v6"  // file 8
SubDocKey(DocKey([], ["k"]), [HT{ physical: 5000 }]) -> "v5"  // file 8
SubDocKey(DocKey([], ["k"]), [HT{ physical: 3000 }]) -> "v3"  // file 3
SubDocKey(DocKey([], ["k"]), [HT{ physical: 2000 }]) -> "v2"  // file 2
SubDocKey(DocKey([], ["k"]), [HT{ physical: 1000 }]) -> "v1"  // file 1
      )#");

  MinorCompaction(5000_usec_ht, /* num_files_to_compact */ 2);
  ASSERT_EQ(3, NumSSTableFiles());
  // Removed the entry at 3000_ms_ht.
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k"]), [HT{ physical: 6000 }]) -> "v6"  // file 9
SubDocKey(DocKey([], ["k"]), [HT{ physical: 5000 }]) -> "v5"  // file 9
SubDocKey(DocKey([], ["k"]), [HT{ physical: 2000 }]) -> "v2"  // file 2
SubDocKey(DocKey([], ["k"]), [HT{ physical: 1000 }]) -> "v1"  // file 1
      )#");

  MinorCompaction(5000_usec_ht, /* num_files_to_compact */ 2);
  ASSERT_EQ(2, NumSSTableFiles());
  // Removed the entry at 2000_ms_ht.
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k"]), [HT{ physical: 6000 }]) -> "v6"  // file 10
SubDocKey(DocKey([], ["k"]), [HT{ physical: 5000 }]) -> "v5"  // file 10
SubDocKey(DocKey([], ["k"]), [HT{ physical: 1000 }]) -> "v1"  // file 1
      )#");

  MinorCompaction(5000_usec_ht, /* num_files_to_compact */ 2);
  ASSERT_EQ(1, NumSSTableFiles());
  // Removed the entry at 2000_ms_ht.
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k"]), [HT{ physical: 6000 }]) -> "v6"  // file 11
SubDocKey(DocKey([], ["k"]), [HT{ physical: 5000 }]) -> "v5"  // file 11
      )#");
}

TEST_P(DocDBTestWrapper, MinorCompactionWithDeletions) {
  ASSERT_OK(DisableCompactions());
  const DocKey doc_key(PrimitiveValues("k"));
  KeyBytes encoded_doc_key(doc_key.Encode());
  for (int i = 1; i <= 6; ++i) {
    auto value_str = Format("v$0", i);
    PrimitiveValue pv = i == 5 ? PrimitiveValue::kTombstone : PrimitiveValue(value_str);
    ASSERT_OK(SetPrimitive(
        DocPath(encoded_doc_key), Value(pv), HybridTime::FromMicros(i * 1000)));
    ASSERT_OK(FlushRocksDbAndWait());
  }

  ASSERT_EQ(6, NumSSTableFiles());
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k"]), [HT{ physical: 6000 }]) -> "v6"  // file 6
SubDocKey(DocKey([], ["k"]), [HT{ physical: 5000 }]) -> DEL   // file 5
SubDocKey(DocKey([], ["k"]), [HT{ physical: 4000 }]) -> "v4"  // file 4
SubDocKey(DocKey([], ["k"]), [HT{ physical: 3000 }]) -> "v3"  // file 3
SubDocKey(DocKey([], ["k"]), [HT{ physical: 2000 }]) -> "v2"  // file 2
SubDocKey(DocKey([], ["k"]), [HT{ physical: 1000 }]) -> "v1"  // file 1
      )#");
  MinorCompaction(5000_usec_ht, /* num_files_to_compact */ 2);

  ASSERT_EQ(5, NumSSTableFiles());
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k"]), [HT{ physical: 6000 }]) -> "v6"  // file 7
SubDocKey(DocKey([], ["k"]), [HT{ physical: 5000 }]) -> DEL   // file 7 as well
SubDocKey(DocKey([], ["k"]), [HT{ physical: 4000 }]) -> "v4"  // file 4
SubDocKey(DocKey([], ["k"]), [HT{ physical: 3000 }]) -> "v3"  // file 3
SubDocKey(DocKey([], ["k"]), [HT{ physical: 2000 }]) -> "v2"  // file 2
SubDocKey(DocKey([], ["k"]), [HT{ physical: 1000 }]) -> "v1"  // file 1
      )#");

  MinorCompaction(5000_usec_ht, /* num_files_to_compact */ 2);
  ASSERT_EQ(4, NumSSTableFiles());
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k"]), [HT{ physical: 6000 }]) -> "v6"  // file 8
SubDocKey(DocKey([], ["k"]), [HT{ physical: 5000 }]) -> DEL   // file 8
SubDocKey(DocKey([], ["k"]), [HT{ physical: 3000 }]) -> "v3"  // file 3
SubDocKey(DocKey([], ["k"]), [HT{ physical: 2000 }]) -> "v2"  // file 2
SubDocKey(DocKey([], ["k"]), [HT{ physical: 1000 }]) -> "v1"  // file 1
      )#");

  MinorCompaction(5000_usec_ht, /* num_files_to_compact */ 2);
  ASSERT_EQ(3, NumSSTableFiles());
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k"]), [HT{ physical: 6000 }]) -> "v6"  // file 9
SubDocKey(DocKey([], ["k"]), [HT{ physical: 5000 }]) -> DEL   // file 9
SubDocKey(DocKey([], ["k"]), [HT{ physical: 2000 }]) -> "v2"  // file 2
SubDocKey(DocKey([], ["k"]), [HT{ physical: 1000 }]) -> "v1"  // file 1
      )#");

  MinorCompaction(5000_usec_ht, /* num_files_to_compact */ 2);
  ASSERT_EQ(2, NumSSTableFiles());
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k"]), [HT{ physical: 6000 }]) -> "v6"  // file 10
SubDocKey(DocKey([], ["k"]), [HT{ physical: 5000 }]) -> DEL   // file 10
SubDocKey(DocKey([], ["k"]), [HT{ physical: 1000 }]) -> "v1"  // file 1
      )#");

  // Now the minor compaction turns into a major compaction and we end up with one file.
  // The tombstone is now gone as well.
  MinorCompaction(5000_usec_ht, /* num_files_to_compact */ 2);
  ASSERT_EQ(1, NumSSTableFiles());
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["k"]), [HT{ physical: 6000 }]) -> "v6"  // file 11
      )#");
}

TEST_P(DocDBTestWrapper, BasicTest) {
  // A few points to make it easier to understand the expected binary representations here:
  // - Initial bytes such as 'S' (kString), 'I' (kInt64) correspond to members of the enum
  //   ValueType.
  // - Strings are terminated with \x00\x00.
  // - Groups of key components in the document key ("hashed" and "range" components) are terminated
  //   with '!' (kGroupEnd).
  // - 64-bit signed integers are encoded in the key using big-endian format with sign bit
  //   inverted.
  // - HybridTimes are represented as 64-bit unsigned integers with all bits inverted, so that's
  //   where we get a lot of \xff bytes from.

  SetInitMarkerBehavior(InitMarkerBehavior::kRequired);

  DocKey string_valued_doc_key(PrimitiveValues("my_key_where_value_is_a_string"));
  ASSERT_STR_EQ_VERBOSE_TRIMMED(
  // Two zeros indicate the end of a string primitive field, and the '!' indicates the end
  // of the "range" part of the DocKey. There is no "hash" part, because the first
  // PrimitiveValue is not a hash value.
      "\"Smy_key_where_value_is_a_string\\x00\\x00!\"",
      string_valued_doc_key.Encode().ToString());

  TestInsertion(
      DocPath(string_valued_doc_key.Encode()),
      PrimitiveValue("value1"),
      1000_usec_ht,
      R"#(1. PutCF('Smy_key_where_value_is_a_string\x00\x00\
                    !', 'Svalue1'))#");

  DocKey doc_key(PrimitiveValues("mydockey", 123456));
  KeyBytes encoded_doc_key(doc_key.Encode());

  TestInsertion(
      DocPath(encoded_doc_key, "subkey_a"),
      PrimitiveValue("value_a"),
      2000_usec_ht,
      R"#(
1. PutCF('Smydockey\x00\x00\
          I\x80\x00\x00\x00\x00\x01\xe2@\
          !', '{')
2. PutCF('Smydockey\x00\x00\
          I\x80\x00\x00\x00\x00\x01\xe2@\
          !\
          Ssubkey_a\x00\x00', 'Svalue_a')
      )#");

  TestInsertion(
      DocPath(encoded_doc_key, "subkey_b", "subkey_c"),
      PrimitiveValue("value_bc"),
      3000_usec_ht,
      R"#(
1. PutCF('Smydockey\x00\x00\
          I\x80\x00\x00\x00\x00\x01\xe2@\
          !\
          Ssubkey_b\x00\x00', '{')
2. PutCF('Smydockey\x00\x00\
          I\x80\x00\x00\x00\x00\x01\xe2@\
          !\
          Ssubkey_b\x00\x00\
          Ssubkey_c\x00\x00', 'Svalue_bc')
      )#");

  // This only has one insertion, because the object at subkey "subkey_b" already exists.
  TestInsertion(
      DocPath(encoded_doc_key, "subkey_b", "subkey_d"),
      PrimitiveValue("value_bd"),
      3500_usec_ht,
      R"#(
1. PutCF('Smydockey\x00\x00\
          I\x80\x00\x00\x00\x00\x01\xe2@\
          !\
          Ssubkey_b\x00\x00\
          Ssubkey_d\x00\x00', 'Svalue_bd')
      )#");

  // Delete a non-existent top-level document. We don't expect any tombstones to be created.
  TestDeletion(
      DocPath(encoded_doc_key, "subkey_x"),
      4000_usec_ht,
      "");

  // Delete a leaf-level value in a subdocument.
  TestDeletion(
      DocPath(encoded_doc_key, "subkey_b", "subkey_c"),
      5000_usec_ht,
      R"#(
1. PutCF('Smydockey\x00\x00\
          I\x80\x00\x00\x00\x00\x01\xe2@\
          !\
          Ssubkey_b\x00\x00\
          Ssubkey_c\x00\x00', 'X')
      )#");

  // Now delete an entire object.
  TestDeletion(
      DocPath(encoded_doc_key, "subkey_b"),
      6000_usec_ht,
      R"#(
1. PutCF('Smydockey\x00\x00\
          I\x80\x00\x00\x00\x00\x01\xe2@\
          !\
          Ssubkey_b\x00\x00', 'X')
      )#");

  // Re-insert a value at subkey_b.subkey_c. This should see the tombstone from the previous
  // operation and create a new object at subkey_b at the new hybrid_time, hence two writes.
  TestInsertion(
      DocPath(encoded_doc_key, "subkey_b", "subkey_c"),
      PrimitiveValue("value_bc_prime"),
      7000_usec_ht,
      R"#(
1. PutCF('Smydockey\x00\x00\
          I\x80\x00\x00\x00\x00\x01\xe2@\
          !\
          Ssubkey_b\x00\x00', '{')
2. PutCF('Smydockey\x00\x00\
          I\x80\x00\x00\x00\x00\x01\xe2@\
          !\
          Ssubkey_b\x00\x00\
          Ssubkey_c\x00\x00', 'Svalue_bc_prime')
      )#");

  // Check the final state of the database.
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(kPredefinedDBStateDebugDumpStr);
  CheckExpectedLatestDBState();

  // Compaction cleanup testing.

  ClearLogicalSnapshots();
  CaptureLogicalSnapshot();
  FullyCompactHistoryBefore(5000_usec_ht);
  // The following entry gets deleted because it is invisible at hybrid_time 5000:
  // SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b", "subkey_c"; HT{ physical: 3000 }])
  //     -> "value_bc"
  //
  // This entry is deleted because we can always remove deletes at or below the cutoff hybrid_time:
  // SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b", "subkey_c"; HT{ physical: 5000 }])
  //     -> DEL
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["my_key_where_value_is_a_string"]), [HT{ physical: 1000 }]) -> "value1"
SubDocKey(DocKey([], ["mydockey", 123456]), [HT{ physical: 2000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_a"; HT{ physical: 2000 w: 1 }]) -> "value_a"
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b"; HT{ physical: 7000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b"; HT{ physical: 6000 }]) -> DEL
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b"; HT{ physical: 3000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b", "subkey_c"; HT{ physical: 7000 w: 1 }]) \
    -> "value_bc_prime"
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b", "subkey_d"; HT{ physical: 3500 }]) -> \
    "value_bd"
      )#");
  CheckExpectedLatestDBState();
  CaptureLogicalSnapshot();
  // Perform the next history compaction starting both from the initial state as well as from the
  // state with the first history compaction (at hybrid_time 5000) already performed.
  for (const auto &snapshot : logical_snapshots()) {
    snapshot.RestoreTo(rocksdb());
    FullyCompactHistoryBefore(6000_usec_ht);
    // Now the following entries get deleted, because the entire subdocument at "subkey_b" gets
    // deleted at hybrid_time 6000, so we won't look at these records if we do a scan at
    // HT{ physical: 6000 }:
    //
    // SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b"; HT{ physical: 3000 }]) -> {}
    // SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b", "subkey_c"; HT{ physical: 5000 }])
    //     -> DEL
    // SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b", "subkey_d"; HT{ physical: 3500 }])
    //     -> "value_bd"
    //
    // And the deletion itself is removed because it is at the history cutoff hybrid_time:
    // SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b"; HT{ physical: 6000 }]) -> DEL
    ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["my_key_where_value_is_a_string"]), [HT{ physical: 1000 }]) -> "value1"
SubDocKey(DocKey([], ["mydockey", 123456]), [HT{ physical: 2000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_a"; HT{ physical: 2000 w: 1 }]) -> "value_a"
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b"; HT{ physical: 7000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b", "subkey_c"; HT{ physical: 7000 w: 1 }]) \
    -> "value_bc_prime"
        )#");
    CheckExpectedLatestDBState();
  }
  CaptureLogicalSnapshot();
  // Also test the next compaction starting with all previously captured states, (1) initial,
  // (2) after a compaction at hybrid_time 5000, and (3) after a compaction at hybrid_time 6000.
  // We are going through snapshots in reverse order so that we end with the initial snapshot that
  // does not have any history trimming done yet.
  for (int i = num_logical_snapshots() - 1; i >= 0; --i) {
    RestoreToRocksDBLogicalSnapshot(i);
    // Test overwriting an entire document with an empty object. This should ideally happen with no
    // reads.
    TestInsertion(
        DocPath(encoded_doc_key),
        PrimitiveValue::kObject,
        8000_usec_ht,
        R"#(
1. PutCF('Smydockey\x00\x00\
          I\x80\x00\x00\x00\x00\x01\xe2@\
          !', '{')
        )#");
    VerifySubDocument(SubDocKey(doc_key), 8000_usec_ht, "{}");
  }

  // Reset our collection of snapshots now that we've performed one more operation.
  ClearLogicalSnapshots();

  CaptureLogicalSnapshot();
  // This is similar to the kPredefinedDBStateDebugDumpStr, but has an additional overwrite of the
  // document with an empty object at hybrid_time 8000.
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["my_key_where_value_is_a_string"]), [HT{ physical: 1000 }]) -> "value1"
SubDocKey(DocKey([], ["mydockey", 123456]), [HT{ physical: 8000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), [HT{ physical: 2000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_a"; HT{ physical: 2000 w: 1 }]) -> "value_a"
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b"; HT{ physical: 7000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b"; HT{ physical: 6000 }]) -> DEL
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b"; HT{ physical: 3000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b", "subkey_c"; HT{ physical: 7000 w: 1 }]) \
    -> "value_bc_prime"
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b", "subkey_c"; HT{ physical: 5000 }]) -> DEL
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b", "subkey_c"; HT{ physical: 3000 w: 1 }]) \
    -> "value_bc"
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b", "subkey_d"; HT{ physical: 3500 }]) -> \
    "value_bd"
      )#");
  FullyCompactHistoryBefore(7999_usec_ht);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["my_key_where_value_is_a_string"]), [HT{ physical: 1000 }]) -> "value1"
SubDocKey(DocKey([], ["mydockey", 123456]), [HT{ physical: 8000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), [HT{ physical: 2000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_a"; HT{ physical: 2000 w: 1 }]) -> "value_a"
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b"; HT{ physical: 7000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey_b", "subkey_c"; HT{ physical: 7000 w: 1 }]) \
    -> "value_bc_prime"
      )#");
  CaptureLogicalSnapshot();
  // Starting with each snapshot, perform the final history compaction and verify we always get the
  // same result.
  for (int i = 0; i < logical_snapshots().size(); ++i) {
    RestoreToRocksDBLogicalSnapshot(i);
    FullyCompactHistoryBefore(8000_usec_ht);
    ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["my_key_where_value_is_a_string"]), [HT{ physical: 1000 }]) -> "value1"
SubDocKey(DocKey([], ["mydockey", 123456]), [HT{ physical: 8000 }]) -> {}
        )#");
  }
}

TEST_P(DocDBTestWrapper, MultiOperationDocWriteBatch) {
  const auto encoded_doc_key = DocKey(PrimitiveValues("a")).Encode();
  auto dwb = MakeDocWriteBatch();
  ASSERT_OK(dwb.SetPrimitive(DocPath(encoded_doc_key, "b"), PrimitiveValue("v1")));
  ASSERT_OK(dwb.SetPrimitive(DocPath(encoded_doc_key, "c", "d"), PrimitiveValue("v2")));
  ASSERT_OK(dwb.SetPrimitive(DocPath(encoded_doc_key, "c", "e"), PrimitiveValue("v3")));

  ASSERT_OK(WriteToRocksDB(dwb, 1000_usec_ht));

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["a"]), ["b"; HT{ physical: 1000 }]) -> "v1"
      SubDocKey(DocKey([], ["a"]), ["c", "d"; HT{ physical: 1000 w: 1 }]) -> "v2"
      SubDocKey(DocKey([], ["a"]), ["c", "e"; HT{ physical: 1000 w: 2 }]) -> "v3"
      )#");

  string dwb_str;
  ASSERT_OK(FormatDocWriteBatch(dwb, &dwb_str));
  EXPECT_STR_EQ_VERBOSE_TRIMMED(
      R"#(
          1. PutCF('Sa\x00\x00!Sb\x00\x00', 'Sv1')
          2. PutCF('Sa\x00\x00!Sc\x00\x00Sd\x00\x00', 'Sv2')
          3. PutCF('Sa\x00\x00!Sc\x00\x00Se\x00\x00', 'Sv3')
      )#", dwb_str);
}

class DocDBTestBoundaryValues: public DocDBTestWrapper {
 protected:
  void TestBoundaryValues(size_t flush_rate) {
    struct Trackers {
      MinMaxTracker<int64_t> key_ints;
      MinMaxTracker<std::string> key_strs;
      MinMaxTracker<HybridTime> times;
    };

    auto dwb = MakeDocWriteBatch();
    constexpr int kTotalRows = 1000;
    constexpr std::mt19937_64::result_type kSeed = 2886476510;

    std::mt19937_64 rng(kSeed);
    std::uniform_int_distribution<int64_t> distribution(0, std::numeric_limits<int64_t>::max());

    std::vector<Trackers> trackers;
    for (int i = 0; i != kTotalRows; ++i) {
      if (i % flush_rate == 0) {
        trackers.emplace_back();
        ASSERT_OK(FlushRocksDbAndWait());
      }
      auto key_str = "key_" + std::to_string(distribution(rng));
      auto key_int = distribution(rng);
      auto value_str = "value_" + std::to_string(distribution(rng));
      auto time = HybridTime::FromMicros(distribution(rng));
      auto key = DocKey(PrimitiveValues(key_str, key_int)).Encode();
      DocPath path(key);
      ASSERT_OK(SetPrimitive(path, PrimitiveValue(value_str), time));
      trackers.back().key_ints(key_int);
      trackers.back().key_strs(key_str);
      trackers.back().times(time);
    }

    string dwb_str;
    ASSERT_OK(FormatDocWriteBatch(dwb, &dwb_str));
    SCOPED_TRACE("\nWrite batch:\n" + dwb_str);
    ASSERT_OK(WriteToRocksDB(dwb, 1000_usec_ht));
    ASSERT_OK(FlushRocksDbAndWait());

    for (auto i = 0; i != 2; ++i) {
      if (i) {
        ASSERT_OK(ReopenRocksDB());
      }
      std::vector<rocksdb::LiveFileMetaData> files;
      rocksdb()->GetLiveFilesMetaData(&files);
      ASSERT_EQ(trackers.size(), files.size());
      sort(files.begin(), files.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.name < rhs.name;
      });

      for (size_t j = 0; j != trackers.size(); ++j) {
        const auto &file = files[j];
        const auto &smallest = file.smallest.user_values;
        const auto &largest = file.largest.user_values;
        {
          auto &times = trackers[j].times;
          DocHybridTime temp;
          ASSERT_OK(GetDocHybridTime(smallest, &temp));
          ASSERT_EQ(times.min, temp.hybrid_time());
          ASSERT_OK(GetDocHybridTime(largest, &temp));
          ASSERT_EQ(times.max, temp.hybrid_time());
        }
        {
          auto &key_ints = trackers[j].key_ints;
          auto &key_strs = trackers[j].key_strs;
          PrimitiveValue temp;
          ASSERT_OK(GetPrimitiveValue(smallest, 0, &temp));
          ASSERT_EQ(PrimitiveValue(key_strs.min), temp);
          ASSERT_OK(GetPrimitiveValue(largest, 0, &temp));
          ASSERT_EQ(PrimitiveValue(key_strs.max), temp);
          ASSERT_OK(GetPrimitiveValue(smallest, 1, &temp));
          ASSERT_EQ(PrimitiveValue(key_ints.min), temp);
          ASSERT_OK(GetPrimitiveValue(largest, 1, &temp));
          ASSERT_EQ(PrimitiveValue(key_ints.max), temp);
        }
      }
    }
  }
};


TEST_F_EX(DocDBTest, BoundaryValues, DocDBTestBoundaryValues) {
  TestBoundaryValues(std::numeric_limits<size_t>::max());
}

TEST_F_EX(DocDBTest, BoundaryValuesMultiFiles, DocDBTestBoundaryValues) {
  TestBoundaryValues(350);
}

TEST_P(DocDBTestWrapper, BloomFilterTest) {
  // Turn off "next instead of seek" optimization, because this test rely on DocDB to do seeks.
  FLAGS_max_nexts_to_avoid_seek = 0;
  // Write batch and flush options.
  auto dwb = MakeDocWriteBatch();
  ASSERT_OK(FlushRocksDbAndWait());

  DocKey key1(0, PrimitiveValues("key1"), PrimitiveValues());
  DocKey key2(0, PrimitiveValues("key2"), PrimitiveValues());
  DocKey key3(0, PrimitiveValues("key3"), PrimitiveValues());
  HybridTime ht;

  SubDocument doc_from_rocksdb;
  bool subdoc_found_in_rocksdb = false;
  int total_bloom_useful = 0;
  int total_table_iterators = 0;

  auto flush_rocksdb = [this, &total_table_iterators]() {
    ASSERT_OK(FlushRocksDbAndWait());
    total_table_iterators =
        regular_db_options().statistics->getTickerCount(rocksdb::NO_TABLE_CACHE_ITERATORS);
  };

  // The following code will set 2/3 keys at a time and flush those 2 writes in a new file. That
  // way we can control and know exactly when the bloom filter is useful.
  // We first write out k1 and k3 and confirm the bloom filter usage is bumped only for checking for
  // k2, as the file does not contain it:
  // file1: k1, k3
  //
  // We then proceed to write k1 and k2 in a new file and check the bloom usage again. At this
  // point, we have:
  // file1: k1, k3
  // file2: k1, k2
  // So the blooms will prune out one file each for k2 and k3 and nothing for k1.
  //
  // Finally, we write out k2 and k3 in a third file, leaving us with:
  // file1: k1, k3
  // file2: k1, k2
  // file3: k2, k3
  // At this point, the blooms will effectively filter out one file for each key.

  dwb.Clear();
  ASSERT_OK(ht.FromUint64(1000));
  ASSERT_OK(dwb.SetPrimitive(DocPath(key1.Encode()), PrimitiveValue("value")));
  ASSERT_OK(dwb.SetPrimitive(DocPath(key3.Encode()), PrimitiveValue("value")));
  ASSERT_OK(WriteToRocksDB(dwb, ht));
  flush_rocksdb();

  auto get_doc = [this, &doc_from_rocksdb, &subdoc_found_in_rocksdb](const DocKey &key) {
    auto encoded_subdoc_key = SubDocKey(key).EncodeWithoutHt();
    GetSubDoc(encoded_subdoc_key, &doc_from_rocksdb, &subdoc_found_in_rocksdb);
  };

  ASSERT_NO_FATALS(CheckBloom(0, &total_bloom_useful, 0, &total_table_iterators));
  ASSERT_NO_FATALS(get_doc(key1));
  ASSERT_TRUE(subdoc_found_in_rocksdb);
  ASSERT_NO_FATALS(CheckBloom(0, &total_bloom_useful, 1, &total_table_iterators));

  ASSERT_NO_FATALS(get_doc(key2));
  ASSERT_TRUE(!subdoc_found_in_rocksdb);
  // Bloom filter excluded this file.
  // docdb::GetSubDocument sometimes seeks twice - first time on key2 and second time to advance
  // out of it, because key2 was found.
  ASSERT_NO_FATALS(CheckBloom(2, &total_bloom_useful, 0, &total_table_iterators));

  ASSERT_NO_FATALS(get_doc(key3));
  ASSERT_TRUE(subdoc_found_in_rocksdb);
  ASSERT_NO_FATALS(CheckBloom(0, &total_bloom_useful, 1, &total_table_iterators));
  dwb.Clear();
  ASSERT_OK(ht.FromUint64(2000));
  ASSERT_OK(dwb.SetPrimitive(DocPath(key1.Encode()), PrimitiveValue("value")));
  ASSERT_OK(dwb.SetPrimitive(DocPath(key2.Encode()), PrimitiveValue("value")));
  ASSERT_OK(WriteToRocksDB(dwb, ht));
  flush_rocksdb();
  ASSERT_NO_FATALS(get_doc(key1));

  ASSERT_NO_FATALS(CheckBloom(0, &total_bloom_useful, 2, &total_table_iterators));
  ASSERT_NO_FATALS(get_doc(key2));
  ASSERT_NO_FATALS(CheckBloom(2, &total_bloom_useful, 1, &total_table_iterators));
  ASSERT_NO_FATALS(get_doc(key3));
  ASSERT_NO_FATALS(CheckBloom(2, &total_bloom_useful, 1, &total_table_iterators));

  dwb.Clear();
  ASSERT_OK(ht.FromUint64(3000));
  ASSERT_OK(dwb.SetPrimitive(DocPath(key2.Encode()), PrimitiveValue("value")));
  ASSERT_OK(dwb.SetPrimitive(DocPath(key3.Encode()), PrimitiveValue("value")));
  ASSERT_OK(WriteToRocksDB(dwb, ht));
  flush_rocksdb();
  ASSERT_NO_FATALS(get_doc(key1));
  ASSERT_NO_FATALS(CheckBloom(2, &total_bloom_useful, 2, &total_table_iterators));
  ASSERT_NO_FATALS(get_doc(key2));
  ASSERT_NO_FATALS(CheckBloom(2, &total_bloom_useful, 2, &total_table_iterators));
  ASSERT_NO_FATALS(get_doc(key3));
  ASSERT_NO_FATALS(CheckBloom(2, &total_bloom_useful, 2, &total_table_iterators));
}

TEST_P(DocDBTestWrapper, BloomFilterCorrectness) {
  // Write batch and flush options.
  auto dwb = MakeDocWriteBatch();
  ASSERT_OK(FlushRocksDbAndWait());

  // We need to write enough keys for fixed-size bloom filter to have more than one block.
  constexpr auto kNumKeys = 100000;
  const ColumnId kColumnId(11);
  const HybridTime ht(1000);

  const auto get_value = [](const int32_t i) {
    return PrimitiveValue::Int32(i);
  };

  const auto get_doc_key = [&](const int32_t i, const bool is_range_key) {
    if (is_range_key) {
      return DocKey({ PrimitiveValue::Int32(i) });
    }
    const auto hash_component = PrimitiveValue::Int32(i);
    auto doc_key = DocKey(i, { hash_component });
    {
      std::string hash_components_buf;
      QLValuePB hash_component_pb;
      PrimitiveValue::ToQLValuePB(
          hash_component, QLType::Create(DataType::INT32), &hash_component_pb);
      AppendToKey(hash_component_pb, &hash_components_buf);
      doc_key.set_hash(YBPartition::HashColumnCompoundValue(hash_components_buf));
    }
    return doc_key;
  };

  const auto get_sub_doc_key = [&](const int32_t i, const bool is_range_key) {
    return SubDocKey(get_doc_key(i, is_range_key), PrimitiveValue(kColumnId));
  };

  for (const auto is_range_key : { false, true }) {
    for (int32_t i = 0; i < kNumKeys; ++i) {
      const auto sub_doc_key = get_sub_doc_key(i, is_range_key);
      const auto value = get_value(i);
      dwb.Clear();
      ASSERT_OK(
          dwb.SetPrimitive(DocPath(sub_doc_key.doc_key().Encode(), sub_doc_key.subkeys()), value));
      ASSERT_OK(WriteToRocksDB(dwb, ht));
    }
    ASSERT_OK(FlushRocksDbAndWait());

    for (int32_t i = 0; i < kNumKeys; ++i) {
      const auto sub_doc_key = get_sub_doc_key(i, is_range_key);
      const auto value = get_value(i);
      const auto encoded_subdoc_key = sub_doc_key.EncodeWithoutHt();
      SubDocument sub_doc;
      bool sub_doc_found;
      GetSubDoc(encoded_subdoc_key, &sub_doc, &sub_doc_found);
      ASSERT_TRUE(sub_doc_found) << "Entry for key #" << i
                                 << " not found, is_range_key: " << is_range_key;
      ASSERT_EQ(static_cast<PrimitiveValue>(sub_doc), value);
    }
  }

  rocksdb::TablePropertiesCollection props;
  rocksdb()->GetPropertiesOfAllTables(&props);
  for (const auto& prop : props) {
    ASSERT_GE(prop.second->num_filter_blocks, 2) << Format(
        "To test rolling over filter block we need at least 2 filter blocks, but got $0 for $1. "
        "Increase kNumKeys in this test.",
        prop.second->num_filter_blocks, prop.first);
  }
}

TEST_P(DocDBTestWrapper, MergingIterator) {
  // Test for the case described in https://yugabyte.atlassian.net/browse/ENG-1677.

  // Turn off "next instead of seek" optimization, because this test rely on DocDB to do seeks.
  FLAGS_max_nexts_to_avoid_seek = 0;

  HybridTime ht;
  ASSERT_OK(ht.FromUint64(1000));

  // Put smaller key into SST file.
  DocKey key1(123, PrimitiveValues("key1"), PrimitiveValues());
  auto dwb = MakeDocWriteBatch();
  ASSERT_OK(dwb.SetPrimitive(DocPath(key1.Encode()), PrimitiveValue("value1")));
  ASSERT_OK(WriteToRocksDB(dwb, ht));
  ASSERT_OK(FlushRocksDbAndWait());

  // Put bigger key into memtable.
  DocKey key2(234, PrimitiveValues("key2"), PrimitiveValues());
  dwb.Clear();
  ASSERT_OK(dwb.SetPrimitive(DocPath(key2.Encode()), PrimitiveValue("value2")));
  ASSERT_OK(WriteToRocksDB(dwb, ht));

  // Get key2 from DocDB. Bloom filter will skip SST file and it should invalidate SST file
  // iterator in order for MergingIterator to not pickup key1 incorrectly.
  VerifySubDocument(SubDocKey(key2), ht, "\"value2\"");
}

TEST_P(DocDBTestWrapper, SetPrimitiveWithInitMarker) {
  // Both required and optional init marker should be ok.
  for (auto init_marker_behavior : kInitMarkerBehaviorList) {
    auto dwb = MakeDocWriteBatch(init_marker_behavior);
    ASSERT_OK(dwb.SetPrimitive(DocPath(kEncodedDocKey1), PrimitiveValue::kObject));
  }
}

TEST_P(DocDBTestWrapper, TestInetSortOrder) {
  InsertInet("1.2.3.4");
  InsertInet("2.2.3.4");
  InsertInet("::1");
  InsertInet("::ffff:ffff");
  InsertInet("::ff:ffff:ffff");
  InsertInet("180::2978:9018:b288:3f6c");
  InsertInet("fe80::2978:9018:b288:3f6c");
  InsertInet("255.255.255.255");
  InsertInet("ffff:ffff::");
  InsertInet("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff");
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["mydockey"]), [::1; HT{ physical: 1000 }]) -> null
SubDocKey(DocKey([], ["mydockey"]), [::255.255.255.255; HT{ physical: 1000 }]) -> null
SubDocKey(DocKey([], ["mydockey"]), [::ff:ffff:ffff; HT{ physical: 1000 }]) -> null
SubDocKey(DocKey([], ["mydockey"]), [1.2.3.4; HT{ physical: 1000 }]) -> null
SubDocKey(DocKey([], ["mydockey"]), [180::2978:9018:b288:3f6c; HT{ physical: 1000 }]) -> null
SubDocKey(DocKey([], ["mydockey"]), [2.2.3.4; HT{ physical: 1000 }]) -> null
SubDocKey(DocKey([], ["mydockey"]), [fe80::2978:9018:b288:3f6c; HT{ physical: 1000 }]) -> null
SubDocKey(DocKey([], ["mydockey"]), [255.255.255.255; HT{ physical: 1000 }]) -> null
SubDocKey(DocKey([], ["mydockey"]), [ffff:ffff::; HT{ physical: 1000 }]) -> null
SubDocKey(DocKey([], ["mydockey"]), [ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff; \
    HT{ physical: 1000 }]) -> null
      )#");
}

TEST_P(DocDBTestWrapper, TestDisambiguationOnWriteId) {
  // Set a column and then delete the entire row in the same write batch. The row disappears.
  auto dwb = MakeDocWriteBatch();
  ASSERT_OK(dwb.SetPrimitive(
      DocPath(kEncodedDocKey1, PrimitiveValue(ColumnId(10))),
      PrimitiveValue("value1")));
  ASSERT_OK(dwb.SetPrimitive(
      DocPath(kEncodedDocKey1), PrimitiveValue::kTombstone));
  ASSERT_OK(WriteToRocksDBAndClear(&dwb, 1000_usec_ht));

  SubDocKey subdoc_key(kDocKey1);
  SubDocument subdoc;
  bool doc_found = false;
  // TODO(dtxn) - check both transaction and non-transaction path?
  auto encoded_subdoc_key = subdoc_key.EncodeWithoutHt();
  GetSubDoc(encoded_subdoc_key, &subdoc, &doc_found, kNonTransactionalOperationContext);
  ASSERT_FALSE(doc_found);

  CaptureLogicalSnapshot();
  for (int cutoff_time_ms = 1000; cutoff_time_ms <= 1001; ++cutoff_time_ms) {
    RestoreToLastLogicalRocksDBSnapshot();

    // The row should still be absent after a compaction.
    // TODO(dtxn) - check both transaction and non-transaction path?
    FullyCompactHistoryBefore(HybridTime::FromMicros(cutoff_time_ms));
    GetSubDoc(encoded_subdoc_key, &subdoc, &doc_found, kNonTransactionalOperationContext);
    ASSERT_FALSE(doc_found);
    ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ("");
  }

  // Delete the row first, and then set a column. This row will exist.
  ASSERT_OK(dwb.SetPrimitive(
      DocPath(kEncodedDocKey2), PrimitiveValue::kTombstone));
  ASSERT_OK(dwb.SetPrimitive(
      DocPath(kEncodedDocKey2, PrimitiveValue(ColumnId(10))),
      PrimitiveValue("value2")));
  ASSERT_OK(WriteToRocksDBAndClear(&dwb, 2000_usec_ht));
  // TODO(dtxn) - check both transaction and non-transaction path?
  SubDocKey subdoc_key2(kDocKey2);
  auto encoded_subdoc_key2 = subdoc_key2.EncodeWithoutHt();
  GetSubDoc(encoded_subdoc_key2, &subdoc, &doc_found, kNonTransactionalOperationContext);
  ASSERT_TRUE(doc_found);

  // The row should still exist after a compaction. The deletion marker should be compacted away.
  CaptureLogicalSnapshot();
  for (int cutoff_time_ms = 2000; cutoff_time_ms <= 2001; ++cutoff_time_ms) {
    RestoreToLastLogicalRocksDBSnapshot();
    FullyCompactHistoryBefore(HybridTime::FromMicros(cutoff_time_ms));
    // TODO(dtxn) - check both transaction and non-transaction path?
    GetSubDoc(encoded_subdoc_key2, &subdoc, &doc_found, kNonTransactionalOperationContext);
    ASSERT_TRUE(doc_found);
    ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(10); HT{ physical: 2000 w: 1 }]) -> "value2"
        )#");
  }
}

TEST_P(DocDBTestWrapper, StaticColumnCompaction) {
  const DocKey hk(0, PrimitiveValues("h1")); // hash key
  const DocKey pk1(hk.hash(), hk.hashed_group(), PrimitiveValues("r1")); // primary key
  const DocKey pk2(hk.hash(), hk.hashed_group(), PrimitiveValues("r2")); //   "      "
  const KeyBytes encoded_hk(hk.Encode());
  const KeyBytes encoded_pk1(pk1.Encode());
  const KeyBytes encoded_pk2(pk2.Encode());

  const MonoDelta one_ms = 1ms;
  const MonoDelta two_ms = 2ms;
  const HybridTime t0 = 1000_usec_ht;
  const HybridTime t1 = server::HybridClock::AddPhysicalTimeToHybridTime(t0, two_ms);
  const HybridTime t2 = server::HybridClock::AddPhysicalTimeToHybridTime(t1, two_ms);

  // Add some static columns: s1 and s2 with TTL, s3 and s4 without.
  ASSERT_OK(SetPrimitive(DocPath(encoded_hk, PrimitiveValue("s1")),
      Value(PrimitiveValue("v1"), one_ms), t0));
  ASSERT_OK(SetPrimitive(DocPath(encoded_hk, PrimitiveValue("s2")),
      Value(PrimitiveValue("v2"), two_ms), t0));
  ASSERT_OK(SetPrimitive(DocPath(encoded_hk, PrimitiveValue("s3")),
      Value(PrimitiveValue("v3old")), t0));
  ASSERT_OK(SetPrimitive(DocPath(encoded_hk, PrimitiveValue("s4")),
      Value(PrimitiveValue("v4")), t0));

  // Add some non-static columns for pk1: c5 and c6 with TTL, c7 and c8 without.
  ASSERT_OK(SetPrimitive(DocPath(encoded_pk1, PrimitiveValue("c5")),
      Value(PrimitiveValue("v51"), one_ms), t0));
  ASSERT_OK(SetPrimitive(DocPath(encoded_pk1, PrimitiveValue("c6")),
      Value(PrimitiveValue("v61"), two_ms), t0));
  ASSERT_OK(SetPrimitive(DocPath(encoded_pk1, PrimitiveValue("c7")),
      Value(PrimitiveValue("v71old")), t0));
  ASSERT_OK(SetPrimitive(DocPath(encoded_pk1, PrimitiveValue("c8")),
      Value(PrimitiveValue("v81")), t0));

  // More non-static columns for another primary key pk2.
  ASSERT_OK(SetPrimitive(DocPath(encoded_pk2, PrimitiveValue("c5")),
      Value(PrimitiveValue("v52"), one_ms), t0));
  ASSERT_OK(SetPrimitive(DocPath(encoded_pk2, PrimitiveValue("c6")),
      Value(PrimitiveValue("v62"), two_ms), t0));
  ASSERT_OK(SetPrimitive(DocPath(encoded_pk2, PrimitiveValue("c7")),
      Value(PrimitiveValue("v72")), t0));
  ASSERT_OK(SetPrimitive(DocPath(encoded_pk2, PrimitiveValue("c8")),
      Value(PrimitiveValue("v82")), t0));

  // Update s3 and delete s4 at t1.
  ASSERT_OK(SetPrimitive(DocPath(encoded_hk, PrimitiveValue("s3")),
      Value(PrimitiveValue("v3new")), t1));
  ASSERT_OK(SetPrimitive(DocPath(encoded_hk, PrimitiveValue("s4")),
      Value(PrimitiveValue::kTombstone), t1));

  // Update c7 of pk1 at t1 also.
  ASSERT_OK(SetPrimitive(DocPath(encoded_pk1, PrimitiveValue("c7")),
      Value(PrimitiveValue("v71new")), t1));

  // Delete c8 of pk2 at t2.
  ASSERT_OK(SetPrimitive(DocPath(encoded_pk2, PrimitiveValue("c8")),
      Value(PrimitiveValue::kTombstone), t2));

  // Verify before compaction.
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey(0x0000, ["h1"], []), ["s1"; HT{ physical: 1000 }]) -> "v1"; ttl: 0.001s
SubDocKey(DocKey(0x0000, ["h1"], []), ["s2"; HT{ physical: 1000 }]) -> "v2"; ttl: 0.002s
SubDocKey(DocKey(0x0000, ["h1"], []), ["s3"; HT{ physical: 3000 }]) -> "v3new"
SubDocKey(DocKey(0x0000, ["h1"], []), ["s3"; HT{ physical: 1000 }]) -> "v3old"
SubDocKey(DocKey(0x0000, ["h1"], []), ["s4"; HT{ physical: 3000 }]) -> DEL
SubDocKey(DocKey(0x0000, ["h1"], []), ["s4"; HT{ physical: 1000 }]) -> "v4"
SubDocKey(DocKey(0x0000, ["h1"], ["r1"]), ["c5"; HT{ physical: 1000 }]) -> "v51"; ttl: 0.001s
SubDocKey(DocKey(0x0000, ["h1"], ["r1"]), ["c6"; HT{ physical: 1000 }]) -> "v61"; ttl: 0.002s
SubDocKey(DocKey(0x0000, ["h1"], ["r1"]), ["c7"; HT{ physical: 3000 }]) -> "v71new"
SubDocKey(DocKey(0x0000, ["h1"], ["r1"]), ["c7"; HT{ physical: 1000 }]) -> "v71old"
SubDocKey(DocKey(0x0000, ["h1"], ["r1"]), ["c8"; HT{ physical: 1000 }]) -> "v81"
SubDocKey(DocKey(0x0000, ["h1"], ["r2"]), ["c5"; HT{ physical: 1000 }]) -> "v52"; ttl: 0.001s
SubDocKey(DocKey(0x0000, ["h1"], ["r2"]), ["c6"; HT{ physical: 1000 }]) -> "v62"; ttl: 0.002s
SubDocKey(DocKey(0x0000, ["h1"], ["r2"]), ["c7"; HT{ physical: 1000 }]) -> "v72"
SubDocKey(DocKey(0x0000, ["h1"], ["r2"]), ["c8"; HT{ physical: 5000 }]) -> DEL
SubDocKey(DocKey(0x0000, ["h1"], ["r2"]), ["c8"; HT{ physical: 1000 }]) -> "v82"
      )#");

  // Compact at t1 = HT{ physical: 3000 }.
  FullyCompactHistoryBefore(t1);

  // Verify after compaction:
  //   s1 -> expired
  //   s4 -> deleted
  //   s3 = v3old -> compacted
  //   pk1.c5 -> expired
  //   pk1.c7 = v71old -> compacted
  //   pk2.c5 -> expired
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey(0x0000, ["h1"], []), ["s2"; HT{ physical: 1000 }]) -> "v2"; ttl: 0.002s
SubDocKey(DocKey(0x0000, ["h1"], []), ["s3"; HT{ physical: 3000 }]) -> "v3new"
SubDocKey(DocKey(0x0000, ["h1"], ["r1"]), ["c6"; HT{ physical: 1000 }]) -> "v61"; ttl: 0.002s
SubDocKey(DocKey(0x0000, ["h1"], ["r1"]), ["c7"; HT{ physical: 3000 }]) -> "v71new"
SubDocKey(DocKey(0x0000, ["h1"], ["r1"]), ["c8"; HT{ physical: 1000 }]) -> "v81"
SubDocKey(DocKey(0x0000, ["h1"], ["r2"]), ["c6"; HT{ physical: 1000 }]) -> "v62"; ttl: 0.002s
SubDocKey(DocKey(0x0000, ["h1"], ["r2"]), ["c7"; HT{ physical: 1000 }]) -> "v72"
SubDocKey(DocKey(0x0000, ["h1"], ["r2"]), ["c8"; HT{ physical: 5000 }]) -> DEL
SubDocKey(DocKey(0x0000, ["h1"], ["r2"]), ["c8"; HT{ physical: 1000 }]) -> "v82"
      )#");
}

TEST_P(DocDBTestWrapper, TestUserTimestamp) {
  const DocKey doc_key(PrimitiveValues("k1"));
  KeyBytes encoded_doc_key(doc_key.Encode());

  // Only optional init marker supported for user timestamp.
  SetInitMarkerBehavior(InitMarkerBehavior::kRequired);
  ASSERT_NOK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue("s10")),
                          Value(PrimitiveValue("v10"), Value::kMaxTtl, 1000),
                          1000_usec_ht));

  SetInitMarkerBehavior(InitMarkerBehavior::kOptional);

  HybridTime ht = 10000_usec_ht;
  // Use same doc_write_batch to test cache.
  auto doc_write_batch = MakeDocWriteBatch();
  ASSERT_OK(doc_write_batch.SetPrimitive(
      DocPath(encoded_doc_key, PrimitiveValue("s1"), PrimitiveValue("s2")),
      Value(PrimitiveValue("v1"), Value::kMaxTtl, 1000)));
  ASSERT_OK(doc_write_batch.SetPrimitive(
      DocPath(encoded_doc_key, PrimitiveValue("s1")),
      Value(PrimitiveValue::kObject, Value::kMaxTtl, 500)));
  ASSERT_OK(WriteToRocksDB(doc_write_batch, ht));

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["k1"]), ["s1"; HT{ physical: 10000 w: 1 }]) -> {}; user timestamp: 500
SubDocKey(DocKey([], ["k1"]), ["s1", "s2"; HT{ physical: 10000 }]) -> "v1"; user timestamp: 1000
      )#");

  doc_write_batch.Clear();
  // Use same doc_write_batch to test cache.
  ASSERT_OK(doc_write_batch.SetPrimitive(
      DocPath(encoded_doc_key, PrimitiveValue("s3")),
      Value(PrimitiveValue::kObject, Value::kMaxTtl, 1000)));
  ASSERT_OK(doc_write_batch.SetPrimitive(
      DocPath(encoded_doc_key, PrimitiveValue("s3"), PrimitiveValue("s4")),
      Value(PrimitiveValue("v1"), Value::kMaxTtl, 500)));
  ASSERT_OK(WriteToRocksDB(doc_write_batch, ht));

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["k1"]), ["s1"; HT{ physical: 10000 w: 1 }]) -> {}; user timestamp: 500
SubDocKey(DocKey([], ["k1"]), ["s1", "s2"; HT{ physical: 10000 }]) -> "v1"; user timestamp: 1000
SubDocKey(DocKey([], ["k1"]), ["s3"; HT{ physical: 10000 }]) -> {}; user timestamp: 1000
      )#");

  doc_write_batch.Clear();
  // Use same doc_write_batch to test cache.
  ASSERT_OK(doc_write_batch.SetPrimitive(
      DocPath(encoded_doc_key, PrimitiveValue("s3"), PrimitiveValue("s4")),
      Value(PrimitiveValue("v1"), Value::kMaxTtl, 2000)));
  ASSERT_OK(doc_write_batch.SetPrimitive(
      DocPath(encoded_doc_key, PrimitiveValue("s3"), PrimitiveValue("s5")),
      Value(PrimitiveValue("v1"), Value::kMaxTtl, 2000)));
  ASSERT_OK(WriteToRocksDB(doc_write_batch, ht));

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["k1"]), ["s1"; HT{ physical: 10000 w: 1 }]) -> {}; user timestamp: 500
SubDocKey(DocKey([], ["k1"]), ["s1", "s2"; HT{ physical: 10000 }]) -> "v1"; user timestamp: 1000
SubDocKey(DocKey([], ["k1"]), ["s3"; HT{ physical: 10000 }]) -> {}; user timestamp: 1000
SubDocKey(DocKey([], ["k1"]), ["s3", "s4"; HT{ physical: 10000 }]) -> "v1"; user timestamp: 2000
SubDocKey(DocKey([], ["k1"]), ["s3", "s5"; HT{ physical: 10000 w: 1 }]) -> "v1"; \
    user timestamp: 2000
      )#");
}

TEST_P(DocDBTestWrapper, TestCompactionWithUserTimestamp) {
  const DocKey doc_key(PrimitiveValues("k1"));
  HybridTime t3000 = 3000_usec_ht;
  HybridTime t5000 = 5000_usec_ht;
  KeyBytes encoded_doc_key(doc_key.Encode());
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue("s1")),
                         Value(PrimitiveValue("v11")), t3000));

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["k1"]), ["s1"; HT{ physical: 3000 }]) -> "v11"
      )#");

  // Delete the row.
  ASSERT_OK(DeleteSubDoc(DocPath(encoded_doc_key, PrimitiveValue("s1")), t5000));
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["k1"]), ["s1"; HT{ physical: 5000 }]) -> DEL
      SubDocKey(DocKey([], ["k1"]), ["s1"; HT{ physical: 3000 }]) -> "v11"
      )#");

  // Try insert with lower timestamp.
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue("s1")),
                         Value(PrimitiveValue("v13"), Value::kMaxTtl, 4000), t3000));

  // No effect on DB.
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["k1"]), ["s1"; HT{ physical: 5000 }]) -> DEL
      SubDocKey(DocKey([], ["k1"]), ["s1"; HT{ physical: 3000 }]) -> "v11"
      )#");

  // Compaction takes away everything.
  FullyCompactHistoryBefore(t5000);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
      )#");

  // Same insert with lower timestamp now works!
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue("s1")),
                         Value(PrimitiveValue("v13"), Value::kMaxTtl, 4000), t3000));
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["k1"]), ["s1"; HT{ physical: 3000 }]) -> "v13"; user timestamp: 4000
      )#");

  // Now try the same with TTL.
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue("s2")),
                         Value(PrimitiveValue("v11"), MonoDelta::FromMicroseconds(1000)), t3000));

  // Insert with TTL.
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["k1"]), ["s1"; HT{ physical: 3000 }]) -> "v13"; user timestamp: 4000
      SubDocKey(DocKey([], ["k1"]), ["s2"; HT{ physical: 3000 }]) -> "v11"; ttl: 0.001s
      )#");

  // Try insert with lower timestamp.
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue("s2")),
                         Value(PrimitiveValue("v13"), Value::kMaxTtl, 2000),
                         t3000,
                         ReadHybridTime::SingleTime(t3000)));

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["k1"]), ["s1"; HT{ physical: 3000 }]) -> "v13"; user timestamp: 4000
      SubDocKey(DocKey([], ["k1"]), ["s2"; HT{ physical: 3000 }]) -> "v11"; ttl: 0.001s
      )#");

  FullyCompactHistoryBefore(t5000);

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["k1"]), ["s1"; HT{ physical: 3000 }]) -> "v13"; user timestamp: 4000
      )#");

  // Insert with lower timestamp after compaction works!
  ASSERT_OK(SetPrimitive(DocPath(encoded_doc_key, PrimitiveValue("s2")),
                         Value(PrimitiveValue("v13"), Value::kMaxTtl, 2000), t3000));
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["k1"]), ["s1"; HT{ physical: 3000 }]) -> "v13"; user timestamp: 4000
      SubDocKey(DocKey([], ["k1"]), ["s2"; HT{ physical: 3000 }]) -> "v13"; user timestamp: 2000
      )#");
}

void QueryBounds(const DocKey& doc_key, int lower, int upper, int base, const DocDB& doc_db,
                 SubDocument* doc_from_rocksdb, bool* subdoc_found,
                 const SubDocKey& subdoc_to_search) {
  HybridTime ht = 1000000_usec_ht;
  auto lower_key =
      SubDocKey(doc_key, PrimitiveValue("subkey" + std::to_string(base + lower))).EncodeWithoutHt();
  SliceKeyBound lower_bound(lower_key, BoundType::kInclusiveLower);
  auto upper_key =
      SubDocKey(doc_key, PrimitiveValue("subkey" + std::to_string(base + upper))).EncodeWithoutHt();
  SliceKeyBound upper_bound(upper_key, BoundType::kInclusiveUpper);
  auto encoded_subdoc_to_search = subdoc_to_search.EncodeWithoutHt();
  GetRedisSubDocumentData data = { encoded_subdoc_to_search, doc_from_rocksdb, subdoc_found };
  data.low_subkey = &lower_bound;
  data.high_subkey = &upper_bound;
  EXPECT_OK(GetRedisSubDocument(
      doc_db, data, rocksdb::kDefaultQueryId,
      kNonTransactionalOperationContext, CoarseTimePoint::max() /* deadline */,
      ReadHybridTime::SingleTime(ht)));
}

void VerifyBounds(SubDocument* doc_from_rocksdb, int lower, int upper, int base) {
  EXPECT_EQ(upper - lower + 1, doc_from_rocksdb->object_num_keys());

  for (int i = lower; i <= upper; i++) {
    SubDocument* subdoc = doc_from_rocksdb->GetChild(
        PrimitiveValue("subkey" + std::to_string(base + i)));
    ASSERT_TRUE(subdoc != nullptr);
    EXPECT_EQ("value" + std::to_string(i), subdoc->GetString());
  }
}

void QueryBoundsAndVerify(const DocKey& doc_key, int lower, int upper, int base,
                          const DocDB& doc_db, const SubDocKey& subdoc_to_search) {
  SubDocument doc_from_rocksdb;
  bool subdoc_found = false;
  QueryBounds(doc_key, lower, upper, base, doc_db, &doc_from_rocksdb, &subdoc_found,
              subdoc_to_search);
  EXPECT_TRUE(subdoc_found);
  VerifyBounds(&doc_from_rocksdb, lower, upper, base);
}

TEST_F(DocDBTestRedis, TestBuildSubDocumentBounds) {
  const DocKey doc_key(PrimitiveValues("key"));
  KeyBytes encoded_doc_key(doc_key.Encode());
  const int nsubkeys = 100;
  const int base = 11000; // To ensure ints can be compared lexicographically.
  string expected_docdb_str;
  AddSubKeys(encoded_doc_key, nsubkeys, base, &expected_docdb_str);

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(expected_docdb_str);

  const SubDocKey subdoc_to_search(doc_key);

  QueryBoundsAndVerify(doc_key, 25, 75, base, doc_db(), subdoc_to_search);
  QueryBoundsAndVerify(doc_key, 50, 60, base, doc_db(), subdoc_to_search);
  QueryBoundsAndVerify(doc_key, 0, nsubkeys - 1, base, doc_db(), subdoc_to_search);

  SubDocument doc_from_rocksdb;
  bool subdoc_found = false;
  QueryBounds(doc_key, -100, 200, base, doc_db(), &doc_from_rocksdb, &subdoc_found,
              subdoc_to_search);
  EXPECT_TRUE(subdoc_found);
  VerifyBounds(&doc_from_rocksdb, 0, nsubkeys - 1, base);

  QueryBounds(doc_key, -100, 50, base, doc_db(), &doc_from_rocksdb, &subdoc_found,
              subdoc_to_search);
  EXPECT_TRUE(subdoc_found);
  VerifyBounds(&doc_from_rocksdb, 0, 50, base);

  QueryBounds(doc_key, 50, 150, base, doc_db(), &doc_from_rocksdb, &subdoc_found,
              subdoc_to_search);
  EXPECT_TRUE(subdoc_found);
  VerifyBounds(&doc_from_rocksdb, 50, nsubkeys - 1, base);

  QueryBounds(doc_key, -100, -50, base, doc_db(), &doc_from_rocksdb, &subdoc_found,
              subdoc_to_search);
  EXPECT_FALSE(subdoc_found);

  QueryBounds(doc_key, 101, 150, base, doc_db(), &doc_from_rocksdb, &subdoc_found,
              subdoc_to_search);
  EXPECT_FALSE(subdoc_found);

  // Try bounds without appropriate doc key.
  QueryBounds(DocKey(PrimitiveValues("abc")), 0, nsubkeys - 1, base, doc_db(), &doc_from_rocksdb,
              &subdoc_found, subdoc_to_search);
  EXPECT_FALSE(subdoc_found);

  // Try bounds different from doc key.
  QueryBounds(doc_key, 0, 99, base, doc_db(), &doc_from_rocksdb, &subdoc_found,
              SubDocKey(DocKey(PrimitiveValues("abc"))));
  EXPECT_FALSE(subdoc_found);

  // Try with bounds pointing to wrong doc key.
  DocKey doc_key_xyz(PrimitiveValues("xyz"));
  AddSubKeys(doc_key_xyz.Encode(), nsubkeys, base, &expected_docdb_str);
  QueryBounds(doc_key_xyz, 0, nsubkeys - 1, base, doc_db(), &doc_from_rocksdb,
              &subdoc_found, subdoc_to_search);
  EXPECT_FALSE(subdoc_found);
}

TEST_P(DocDBTestWrapper, TestCompactionForCollectionsWithTTL) {
  DocKey collection_key(PrimitiveValues("collection"));
  SetUpCollectionWithTTL(collection_key, UseIntermediateFlushes::kFalse);

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(ExpectedDebugDumpForCollectionWithTTL(
      collection_key, InitMarkerExpired::kFalse));

  FullyCompactHistoryBefore(HybridTime::FromMicros(1050 + 10 * 1000000));
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(ExpectedDebugDumpForCollectionWithTTL(
      collection_key, InitMarkerExpired::kTrue));

  const auto subdoc_key = SubDocKey(collection_key).EncodeWithoutHt();
  SubDocument doc_from_rocksdb;
  bool subdoc_found_in_rocksdb = false;
  GetSubDoc(
      subdoc_key, &doc_from_rocksdb, &subdoc_found_in_rocksdb, kNonTransactionalOperationContext,
      ReadHybridTime::FromMicros(1200));
  ASSERT_TRUE(subdoc_found_in_rocksdb);

  for (int i = 0; i < kNumSubKeysForCollectionsWithTTL * 2; i++) {
    SubDocument subdoc;
    string key = "k" + std::to_string(i);
    string value = "vv" + std::to_string(i);
    ASSERT_EQ(value, doc_from_rocksdb.GetChild(PrimitiveValue(key))->GetString());
  }
}

TEST_P(DocDBTestWrapper, MinorCompactionsForCollectionsWithTTL) {
  ASSERT_OK(DisableCompactions());
  DocKey collection_key(PrimitiveValues("c"));
  SetUpCollectionWithTTL(collection_key, UseIntermediateFlushes::kTrue);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      ExpectedDebugDumpForCollectionWithTTL(collection_key, InitMarkerExpired::kFalse));
  MinorCompaction(
      HybridTime::FromMicros(1100 + 20 * 1000000 + 1), /* num_files_to_compact */ 2,
      /* start_index */ 1);

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["c"]), [HT{ physical: 1000 }]) -> {}; ttl: 10.000s               // file 1
SubDocKey(DocKey([], ["c"]), ["k0"; HT{ physical: 1100 }]) -> DEL                      // file 8
SubDocKey(DocKey([], ["c"]), ["k0"; HT{ physical: 1000 w: 1 }]) -> "v0"; ttl: 10.000s  // file 1
SubDocKey(DocKey([], ["c"]), ["k1"; HT{ physical: 1100 }]) -> "vv1"; ttl: 21.000s      // file 8
SubDocKey(DocKey([], ["c"]), ["k1"; HT{ physical: 1000 w: 2 }]) -> "v1"; ttl: 10.000s  // file 1
SubDocKey(DocKey([], ["c"]), ["k2"; HT{ physical: 1100 }]) -> "vv2"; ttl: 22.000s      // file 4
SubDocKey(DocKey([], ["c"]), ["k2"; HT{ physical: 1000 w: 3 }]) -> "v2"; ttl: 10.000s  // file 1
SubDocKey(DocKey([], ["c"]), ["k3"; HT{ physical: 1100 }]) -> "vv3"; ttl: 23.000s      // file 5
SubDocKey(DocKey([], ["c"]), ["k4"; HT{ physical: 1100 }]) -> "vv4"; ttl: 24.000s      // file 6
SubDocKey(DocKey([], ["c"]), ["k5"; HT{ physical: 1100 }]) -> "vv5"; ttl: 25.000s      // file 7
  )#");

  // Compact files 4, 5, 6, 7, 8. This should result in creation of a number of delete markers
  // from expired entries. Some expired entries from the first file will stay.
  MinorCompaction(
      HybridTime::FromMicros(1100 + 24 * 1000000 + 1), /* num_files_to_compact */ 5,
      /* start_index */ 1);

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["c"]), [HT{ physical: 1000 }]) -> {}; ttl: 10.000s               // file 1
SubDocKey(DocKey([], ["c"]), ["k0"; HT{ physical: 1100 }]) -> DEL                      // file 9
SubDocKey(DocKey([], ["c"]), ["k0"; HT{ physical: 1000 w: 1 }]) -> "v0"; ttl: 10.000s  // file 1
SubDocKey(DocKey([], ["c"]), ["k1"; HT{ physical: 1100 }]) -> DEL                      // file 9
SubDocKey(DocKey([], ["c"]), ["k1"; HT{ physical: 1000 w: 2 }]) -> "v1"; ttl: 10.000s  // file 1
SubDocKey(DocKey([], ["c"]), ["k2"; HT{ physical: 1100 }]) -> DEL                      // file 9
SubDocKey(DocKey([], ["c"]), ["k2"; HT{ physical: 1000 w: 3 }]) -> "v2"; ttl: 10.000s  // file 1
SubDocKey(DocKey([], ["c"]), ["k3"; HT{ physical: 1100 }]) -> DEL                      // file 9
SubDocKey(DocKey([], ["c"]), ["k4"; HT{ physical: 1100 }]) -> DEL                      // file 9
SubDocKey(DocKey([], ["c"]), ["k5"; HT{ physical: 1100 }]) -> "vv5"; ttl: 25.000s      // file 9
  )#");

}

TEST_P(DocDBTestWrapper, CompactionWithTransactions) {
  FLAGS_TEST_docdb_sort_weak_intents_in_tests = true;

  const DocKey doc_key(PrimitiveValues("mydockey", 123456));
  KeyBytes encoded_doc_key(doc_key.Encode());
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key), PrimitiveValue::kObject, 1000_usec_ht));
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key, "subkey1"), PrimitiveValue("value1"), 1000_usec_ht));
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key, "subkey1"), PrimitiveValue("value2"), 2000_usec_ht));
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key, "subkey1"), PrimitiveValue("value3"), 3000_usec_ht));
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key), PrimitiveValue::kObject, 4000_usec_ht));

  SetTransactionIsolationLevel(IsolationLevel::SNAPSHOT_ISOLATION);

  Result<TransactionId> txn1 = FullyDecodeTransactionId("0000000000000001");
  const auto kTxn1HT = 5000_usec_ht;
  ASSERT_OK(txn1);
  SetCurrentTransactionId(*txn1);
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key), PrimitiveValue::kObject, kTxn1HT));
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key, "subkey1"), PrimitiveValue("value4"), kTxn1HT));

  Result<TransactionId> txn2 = FullyDecodeTransactionId("0000000000000002");
  const auto kTxn2HT = 6000_usec_ht;
  ASSERT_OK(txn2);
  SetCurrentTransactionId(*txn2);
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key), PrimitiveValue::kObject, kTxn2HT));
  ASSERT_OK(SetPrimitive(
      DocPath(encoded_doc_key, "subkey2"), PrimitiveValue("value5"), kTxn2HT));

  ResetCurrentTransactionId();
  TransactionId txn3 = ASSERT_RESULT(FullyDecodeTransactionId("0000000000000003"));
  const auto kTxn3HT = 7000_usec_ht;
  std::vector<ExternalIntent> intents = {
    { DocPath(encoded_doc_key, "subkey3"), Value(PrimitiveValue("value6")) },
    { DocPath(encoded_doc_key, "subkey4"), Value(PrimitiveValue("value7")) }
  };
  Uuid status_tablet;
  ASSERT_OK(status_tablet.FromString("4c3e1d91-5ea7-4449-8bb3-8b0a3f9ae903"));
  ASSERT_OK(AddExternalIntents(txn3, intents, status_tablet, kTxn3HT));

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
SubDocKey(DocKey([], ["mydockey", 123456]), [HT{ physical: 4000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), [HT{ physical: 1000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey1"; HT{ physical: 3000 }]) -> "value3"
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey1"; HT{ physical: 2000 }]) -> "value2"
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey1"; HT{ physical: 1000 }]) -> "value1"
TXN EXT 30303030-3030-3030-3030-303030303033 HT{ physical: 7000 } -> \
    IT 03e99a3f0a8bb38b4944a75e911d3e4c [\
    SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey3"]) -> "value6", \
    SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey4"]) -> "value7"]
SubDocKey(DocKey([], []), []) [kWeakRead, kWeakWrite] HT{ physical: 6000 w: 1 } -> \
    TransactionId(30303030-3030-3030-3030-303030303032) none
SubDocKey(DocKey([], []), []) [kWeakRead, kWeakWrite] HT{ physical: 5000 w: 1 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["mydockey"]), []) [kWeakRead, kWeakWrite] HT{ physical: 6000 w: 2 } -> \
    TransactionId(30303030-3030-3030-3030-303030303032) none
SubDocKey(DocKey([], ["mydockey"]), []) [kWeakRead, kWeakWrite] HT{ physical: 5000 w: 2 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["mydockey", 123456]), []) [kWeakRead, kWeakWrite] HT{ physical: 6000 w: 3 } \
    -> TransactionId(30303030-3030-3030-3030-303030303032) none
SubDocKey(DocKey([], ["mydockey", 123456]), []) [kWeakRead, kWeakWrite] HT{ physical: 5000 w: 3 } \
    -> TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["mydockey", 123456]), []) [kStrongRead, kStrongWrite] HT{ physical: 6000 } \
    -> TransactionId(30303030-3030-3030-3030-303030303032) WriteId(2) {}
SubDocKey(DocKey([], ["mydockey", 123456]), []) [kStrongRead, kStrongWrite] HT{ physical: 5000 } \
    -> TransactionId(30303030-3030-3030-3030-303030303031) WriteId(0) {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey1"]) [kStrongRead, kStrongWrite] \
    HT{ physical: 5000 } -> TransactionId(30303030-3030-3030-3030-303030303031) WriteId(1) "value4"
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey2"]) [kStrongRead, kStrongWrite] \
    HT{ physical: 6000 } -> TransactionId(30303030-3030-3030-3030-303030303032) WriteId(3) "value5"
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 5000 } -> \
    SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey1"]) [kStrongRead, kStrongWrite] \
    HT{ physical: 5000 }
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 5000 w: 1 } -> \
    SubDocKey(DocKey([], []), []) [kWeakRead, kWeakWrite] HT{ physical: 5000 w: 1 }
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 5000 w: 2 } -> \
    SubDocKey(DocKey([], ["mydockey"]), []) [kWeakRead, kWeakWrite] HT{ physical: 5000 w: 2 }
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 5000 w: 3 } -> \
    SubDocKey(DocKey([], ["mydockey", 123456]), []) [kWeakRead, kWeakWrite] \
    HT{ physical: 5000 w: 3 }
TXN REV 30303030-3030-3030-3030-303030303032 HT{ physical: 6000 } -> \
    SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey2"]) [kStrongRead, kStrongWrite] \
    HT{ physical: 6000 }
TXN REV 30303030-3030-3030-3030-303030303032 HT{ physical: 6000 w: 1 } -> \
    SubDocKey(DocKey([], []), []) [kWeakRead, kWeakWrite] HT{ physical: 6000 w: 1 }
TXN REV 30303030-3030-3030-3030-303030303032 HT{ physical: 6000 w: 2 } -> \
    SubDocKey(DocKey([], ["mydockey"]), []) [kWeakRead, kWeakWrite] HT{ physical: 6000 w: 2 }
TXN REV 30303030-3030-3030-3030-303030303032 HT{ physical: 6000 w: 3 } -> \
    SubDocKey(DocKey([], ["mydockey", 123456]), []) [kWeakRead, kWeakWrite] \
    HT{ physical: 6000 w: 3 }
    )#");
  FullyCompactHistoryBefore(3500_usec_ht);
  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(
      R"#(
SubDocKey(DocKey([], ["mydockey", 123456]), [HT{ physical: 4000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), [HT{ physical: 1000 }]) -> {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey1"; HT{ physical: 3000 }]) -> "value3"
TXN EXT 30303030-3030-3030-3030-303030303033 HT{ physical: 7000 } -> \
    IT 03e99a3f0a8bb38b4944a75e911d3e4c [\
    SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey3"]) -> "value6", \
    SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey4"]) -> "value7"]
SubDocKey(DocKey([], []), []) [kWeakRead, kWeakWrite] HT{ physical: 6000 w: 1 } -> \
    TransactionId(30303030-3030-3030-3030-303030303032) none
SubDocKey(DocKey([], []), []) [kWeakRead, kWeakWrite] HT{ physical: 5000 w: 1 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["mydockey"]), []) [kWeakRead, kWeakWrite] HT{ physical: 6000 w: 2 } -> \
    TransactionId(30303030-3030-3030-3030-303030303032) none
SubDocKey(DocKey([], ["mydockey"]), []) [kWeakRead, kWeakWrite] HT{ physical: 5000 w: 2 } -> \
    TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["mydockey", 123456]), []) [kWeakRead, kWeakWrite] HT{ physical: 6000 w: 3 } \
    -> TransactionId(30303030-3030-3030-3030-303030303032) none
SubDocKey(DocKey([], ["mydockey", 123456]), []) [kWeakRead, kWeakWrite] HT{ physical: 5000 w: 3 } \
    -> TransactionId(30303030-3030-3030-3030-303030303031) none
SubDocKey(DocKey([], ["mydockey", 123456]), []) [kStrongRead, kStrongWrite] HT{ physical: 6000 } \
    -> TransactionId(30303030-3030-3030-3030-303030303032) WriteId(2) {}
SubDocKey(DocKey([], ["mydockey", 123456]), []) [kStrongRead, kStrongWrite] HT{ physical: 5000 } \
    -> TransactionId(30303030-3030-3030-3030-303030303031) WriteId(0) {}
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey1"]) [kStrongRead, kStrongWrite] \
    HT{ physical: 5000 } -> TransactionId(30303030-3030-3030-3030-303030303031) WriteId(1) "value4"
SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey2"]) [kStrongRead, kStrongWrite] \
    HT{ physical: 6000 } -> TransactionId(30303030-3030-3030-3030-303030303032) WriteId(3) "value5"
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 5000 } -> \
    SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey1"]) [kStrongRead, kStrongWrite] \
    HT{ physical: 5000 }
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 5000 w: 1 } -> \
    SubDocKey(DocKey([], []), []) [kWeakRead, kWeakWrite] HT{ physical: 5000 w: 1 }
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 5000 w: 2 } -> \
    SubDocKey(DocKey([], ["mydockey"]), []) [kWeakRead, kWeakWrite] HT{ physical: 5000 w: 2 }
TXN REV 30303030-3030-3030-3030-303030303031 HT{ physical: 5000 w: 3 } -> \
    SubDocKey(DocKey([], ["mydockey", 123456]), []) [kWeakRead, kWeakWrite] \
    HT{ physical: 5000 w: 3 }
TXN REV 30303030-3030-3030-3030-303030303032 HT{ physical: 6000 } -> \
    SubDocKey(DocKey([], ["mydockey", 123456]), ["subkey2"]) [kStrongRead, kStrongWrite] \
    HT{ physical: 6000 }
TXN REV 30303030-3030-3030-3030-303030303032 HT{ physical: 6000 w: 1 } -> \
    SubDocKey(DocKey([], []), []) [kWeakRead, kWeakWrite] HT{ physical: 6000 w: 1 }
TXN REV 30303030-3030-3030-3030-303030303032 HT{ physical: 6000 w: 2 } -> \
    SubDocKey(DocKey([], ["mydockey"]), []) [kWeakRead, kWeakWrite] HT{ physical: 6000 w: 2 }
TXN REV 30303030-3030-3030-3030-303030303032 HT{ physical: 6000 w: 3 } -> \
    SubDocKey(DocKey([], ["mydockey", 123456]), []) [kWeakRead, kWeakWrite] \
    HT{ physical: 6000 w: 3 }
    )#");
}

TEST_P(DocDBTestWrapper, ForceFlushedFrontier) {
  // We run with compactions disabled, because they may interefere with force-setting the OpId.
  ASSERT_OK(DisableCompactions());
  op_id_ = {1, 1};
  rocksdb::UserFrontierPtr flushed_frontier;
  for (int i = 1; i < 20; ++i) {
    const DocKey doc_key(PrimitiveValues(i));
    const KeyBytes encoded_doc_key = doc_key.Encode();
    SetupRocksDBState(encoded_doc_key);
    ASSERT_OK(FlushRocksDbAndWait());
    flushed_frontier = rocksdb()->GetFlushedFrontier();
    LOG(INFO) << "Flushed frontier after i=" << i << ": "
              << (flushed_frontier ? flushed_frontier->ToString() : "N/A");
  }
  ASSERT_TRUE(flushed_frontier.get() != nullptr);
  ConsensusFrontier consensus_frontier =
      down_cast<ConsensusFrontier&>(*flushed_frontier);
  ConsensusFrontier new_consensus_frontier = consensus_frontier;
  new_consensus_frontier.set_op_id({
      consensus_frontier.op_id().term,
      consensus_frontier.op_id().index / 2
  });
  ASSERT_EQ(new_consensus_frontier.op_id().term, consensus_frontier.op_id().term);
  ASSERT_LT(new_consensus_frontier.op_id().index, consensus_frontier.op_id().index);
  ASSERT_EQ(new_consensus_frontier.hybrid_time(), consensus_frontier.hybrid_time());
  ASSERT_EQ(new_consensus_frontier.history_cutoff(), consensus_frontier.history_cutoff());
  rocksdb::UserFrontierPtr new_user_frontier_ptr(new ConsensusFrontier(new_consensus_frontier));

  LOG(INFO) << "Attempting to change flushed frontier from " << consensus_frontier
            << " to " << new_consensus_frontier;
  ASSERT_OK(regular_db_->ModifyFlushedFrontier(
      new_user_frontier_ptr, rocksdb::FrontierModificationMode::kForce));
  LOG(INFO) << "Checking that flushed froniter was set to " << new_consensus_frontier;
  ASSERT_EQ(*new_user_frontier_ptr, *regular_db_->GetFlushedFrontier());

  LOG(INFO) << "Reopening RocksDB";
  ASSERT_OK(ReopenRocksDB());
  LOG(INFO) << "Checking that flushed frontier is still set to "
            << regular_db_->GetFlushedFrontier()->ToString();
  ASSERT_EQ(*new_user_frontier_ptr, *regular_db_->GetFlushedFrontier());
}

// Handy code to analyze some DB.
TEST_P(DocDBTestWrapper, DISABLED_DumpDB) {
  tablet::TabletOptions tablet_options;
  rocksdb::Options options;
  docdb::InitRocksDBOptions(
      &options, "" /* log_prefix */, rocksdb::CreateDBStatistics(), tablet_options);

  rocksdb::DB* rocksdb = nullptr;
  std::string db_path = "";
  ASSERT_OK(rocksdb::DB::Open(options, db_path, &rocksdb));

  rocksdb::ReadOptions read_opts;
  read_opts.query_id = rocksdb::kDefaultQueryId;
  unique_ptr<rocksdb::Iterator> iter(rocksdb->NewIterator(read_opts));
  iter->SeekToFirst();

  int txn_meta = 0;
  int rev_key = 0;
  int intent = 0;
  while (iter->Valid()) {
    auto key_type = GetKeyType(iter->key(), StorageDbType::kIntents);
    if (key_type == KeyType::kTransactionMetadata) {
      ++txn_meta;
    } else if (key_type == KeyType::kReverseTxnKey) {
      ++rev_key;
    } else if (key_type == KeyType::kIntentKey) {
      ++intent;
    } else {
      ASSERT_TRUE(false);
    }
    iter->Next();
  }

  LOG(INFO) << "TXN meta: " << txn_meta << ", rev key: " << rev_key << ", intents: " << intent;
}

TEST_P(DocDBTestWrapper, SetHybridTimeFilter) {
  auto dwb = MakeDocWriteBatch();
  for (int i = 1; i <= 4; ++i) {
    ASSERT_OK(WriteSimple(i));
  }

  ASSERT_OK(FlushRocksDbAndWait());

  CloseRocksDB();

  RocksDBPatcher patcher(rocksdb_dir_, regular_db_options_);

  ASSERT_OK(patcher.Load());
  ASSERT_OK(patcher.SetHybridTimeFilter(HybridTime::FromMicros(2000)));

  ASSERT_OK(OpenRocksDB());

  ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(10); HT{ physical: 1000 }]) -> 1
      SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(10); HT{ physical: 2000 }]) -> 2
  )#");

  ASSERT_OK(WriteSimple(5));

  for (int j = 0; j < 3; ++j) {
    SCOPED_TRACE(Format("Iteration $0", j));

    ASSERT_DOC_DB_DEBUG_DUMP_STR_EQ(R"#(
      SubDocKey(DocKey([], ["row1", 11111]), [ColumnId(10); HT{ physical: 1000 }]) -> 1
      SubDocKey(DocKey([], ["row2", 22222]), [ColumnId(10); HT{ physical: 2000 }]) -> 2
      SubDocKey(DocKey([], ["row5", 55555]), [ColumnId(10); HT{ physical: 5000 }]) -> 5
    )#");

    if (j == 0) {
      ASSERT_OK(FlushRocksDbAndWait());
    } else if (j == 1) {
      ASSERT_OK(ForceRocksDBCompact(rocksdb()));
    }
  }

}

void Append(const char* a, const char* b, std::string* out) {
  out->append(a, b);
}

void PushBack(const std::string& value, std::vector<std::string>* out) {
  out->push_back(value);
}

void Append(const char* a, const char* b, faststring* out) {
  out->append(a, b - a);
}

void PushBack(const faststring& value, std::vector<std::string>* out) {
  out->emplace_back(value.c_str(), value.size());
}

void Append(const char* a, const char* b, boost::container::small_vector_base<char>* out) {
  out->insert(out->end(), a, b);
}

void PushBack(
    const boost::container::small_vector_base<char>& value, std::vector<std::string>* out) {
  out->emplace_back(value.begin(), value.end());
}

template <size_t SmallLen>
void Append(const char* a, const char* b, ByteBuffer<SmallLen>* out) {
  out->Append(a, b);
}

template <size_t SmallLen>
void PushBack(const ByteBuffer<SmallLen>& value, std::vector<std::string>* out) {
  out->push_back(value.ToString());
}

constexpr size_t kSourceLen = 32;
const std::string kSource = RandomHumanReadableString(kSourceLen);

template <class T>
void TestKeyBytes(const char* title, std::vector<std::string>* out = nullptr) {
#ifdef NDEBUG
  constexpr size_t kIterations = 100000000;
#else
  constexpr size_t kIterations = RegularBuildVsSanitizers(10000000, 100000);
#endif
  const char* source_start = kSource.c_str();

  auto start = GetThreadCpuTimeMicros();
  T key_bytes;
  for (size_t i = kIterations; i-- > 0;) {
    key_bytes.clear();
    const char* a = source_start + ((i * 102191ULL) & (kSourceLen - 1ULL));
    const char* b = source_start + ((i * 99191ULL) & (kSourceLen - 1ULL));
    Append(std::min(a, b), std::max(a, b) + 1, &key_bytes);
    a = source_start + ((i * 88937ULL) & (kSourceLen - 1ULL));
    b = source_start + ((i * 74231ULL) & (kSourceLen - 1ULL));
    Append(std::min(a, b), std::max(a, b) + 1, &key_bytes);
    a = source_start + ((i * 75983ULL) & (kSourceLen - 1ULL));
    b = source_start + ((i * 72977ULL) & (kSourceLen - 1ULL));
    Append(std::min(a, b), std::max(a, b) + 1, &key_bytes);
    if (out) {
      PushBack(key_bytes, out);
    }
  }
  auto time = MonoDelta::FromMicroseconds(GetThreadCpuTimeMicros() - start);
  LOG(INFO) << title << ": " << time;
}

TEST_P(DocDBTestWrapper, DISABLED_KeyBuffer) {
  TestKeyBytes<std::string>("std::string");
  TestKeyBytes<faststring>("faststring");
  TestKeyBytes<boost::container::small_vector<char, 8>>("small_vector<char, 8>");
  TestKeyBytes<boost::container::small_vector<char, 16>>("small_vector<char, 16>");
  TestKeyBytes<boost::container::small_vector<char, 32>>("small_vector<char, 32>");
  TestKeyBytes<boost::container::small_vector<char, 64>>("small_vector<char, 64>");
  TestKeyBytes<ByteBuffer<8>>("ByteBuffer<8>");
  TestKeyBytes<ByteBuffer<16>>("ByteBuffer<16>");
  TestKeyBytes<ByteBuffer<32>>("ByteBuffer<32>");
  TestKeyBytes<ByteBuffer<64>>("ByteBuffer<64>");
}

}  // namespace docdb
}  // namespace yb
