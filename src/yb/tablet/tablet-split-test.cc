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

#include <boost/algorithm/string/join.hpp>

#include "yb/common/ql_protocol_util.h"
#include "yb/common/ql_value.h"

#include "yb/docdb/docdb.h"
#include "yb/docdb/docdb_debug.h"

#include "yb/tablet/tablet-test-util.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/local_tablet_writer.h"
#include "yb/util/random_util.h"
#include "yb/util/size_literals.h"

DECLARE_int64(db_write_buffer_size);
DECLARE_bool(rocksdb_disable_compactions);
DECLARE_int32(rocksdb_level0_file_num_compaction_trigger);

namespace yb {
namespace tablet {

class TabletSplitTest : public YBTabletTest {
 public:
  TabletSplitTest() : YBTabletTest(Schema({ ColumnSchema("key", INT32, false, true),
                                            ColumnSchema("val", STRING) },
                                          1)) {}

  void SetUp() override {
    FLAGS_db_write_buffer_size = 1_MB;
    FLAGS_rocksdb_level0_file_num_compaction_trigger = -1;
    YBTabletTest::SetUp();
    writer_.reset(new LocalTabletWriter(tablet().get()));
  }

 protected:

  docdb::DocKeyHash InsertRow(int key, const std::string& val, LocalTabletWriter::Batch* batch) {
    QLWriteRequestPB* req = batch->Add();
    req->set_type(QLWriteRequestPB::QL_STMT_INSERT);
    QLAddInt32HashValue(req, key);
    QLAddStringColumnValue(req, kFirstColumnId + 1, val);
    QLSetHashCode(req);
    return req->hash_code();
  }

  Result<std::vector<QLRow>> SelectAll(Tablet* tablet) {
    ReadHybridTime read_time = ReadHybridTime::SingleTime(VERIFY_RESULT(tablet->SafeTime()));
    QLReadRequestPB req;
    QLAddColumns(schema_, {}, &req);
    QLReadRequestResult result;
    EXPECT_OK(tablet->HandleQLReadRequest(
        CoarseTimePoint::max(), read_time, req, TransactionMetadataPB(), &result));

    EXPECT_EQ(QLResponsePB::YQL_STATUS_OK, result.response.status());

    return CreateRowBlock(QLClient::YQL_CLIENT_CQL, schema_, result.rows_data)->rows();
  }

  docdb::DocKeyHash GetRowHashCode(const QLRow& row) {
    std::string tmp;
    AppendToKey(row.column(0).value(), &tmp);
    return YBPartition::HashColumnCompoundValue(tmp);
  }

  std::unique_ptr<LocalTabletWriter> writer_;
};

namespace {

boost::optional<docdb::DocKeyHash> PartitionKeyToHash(const std::string& partition_key) {
  if (partition_key.empty()) {
    return boost::none;
  } else {
    return PartitionSchema::DecodeMultiColumnHashValue(partition_key);
  }
}

} // namespace

TEST_F(TabletSplitTest, SplitTablet) {
  constexpr auto kNumRows = 10000;
  constexpr auto kValuePrefixLength = 1024;
  constexpr auto kRowsPerSourceFlush = kNumRows / 7;
  constexpr auto kNumSplits = 5;

  const auto value_format = RandomHumanReadableString(kValuePrefixLength) + "_$0";
  docdb::DocKeyHash min_hash_code = std::numeric_limits<docdb::DocKeyHash>::max();
  docdb::DocKeyHash max_hash_code = std::numeric_limits<docdb::DocKeyHash>::min();
  {
    LocalTabletWriter::Batch batch;
    for (auto i = 1; i <= kNumRows; ++i) {
      const auto hash_code = InsertRow(i, Format(value_format, i), &batch);
      min_hash_code = std::min(min_hash_code, hash_code);
      max_hash_code = std::max(max_hash_code, hash_code);
      if (i % kRowsPerSourceFlush == 0) {
        ASSERT_OK(writer_->WriteBatch(&batch));
        batch.Clear();
        ASSERT_OK(tablet()->Flush(FlushMode::kSync));
      }
    }
    if (!batch.empty()) {
      ASSERT_OK(writer_->WriteBatch(&batch));
    }
  }

  VLOG(1) << "Source tablet:" << std::endl
          << docdb::DocDBDebugDumpToStr(tablet()->doc_db(), docdb::IncludeBinary::kTrue);
  const auto source_docdb_dump_str = tablet()->TEST_DocDBDumpStr(IncludeIntents::kTrue);
  std::unordered_set<std::string> source_docdb_dump;
  tablet()->TEST_DocDBDumpToContainer(IncludeIntents::kTrue, &source_docdb_dump);

  std::unordered_set<std::string> source_rows;
  for (const auto& row : ASSERT_RESULT(SelectAll(tablet().get()))) {
    source_rows.insert(row.ToString());
  }
  auto source_rows2 = source_rows;

  std::vector<TabletPtr> split_tablets;

  std::shared_ptr<Partition> partition = tablet()->metadata()->partition();
  docdb::KeyBounds key_bounds;
  for (auto i = 1; i <= kNumSplits + 1; ++i) {
    const auto subtablet_id = Format("$0-sub-$1", tablet()->tablet_id(), yb::ToString(i));

    // Last sub tablet will contain only one hash to explicitly test this case.
    if (i <= kNumSplits) {
      const docdb::DocKeyHash split_hash_code =
          min_hash_code + i * static_cast<uint32>(max_hash_code - min_hash_code) / kNumSplits;
      LOG(INFO) << "Split hash code: " << split_hash_code;
      const auto partition_key = PartitionSchema::EncodeMultiColumnHashValue(split_hash_code);
      docdb::KeyBytes encoded_doc_key;
      docdb::DocKeyEncoderAfterTableIdStep(&encoded_doc_key).Hash(
          split_hash_code, std::vector<docdb::PrimitiveValue>());
      partition->set_partition_key_end(partition_key);
      key_bounds.upper = encoded_doc_key;
    } else {
      partition->set_partition_key_end("");
      key_bounds.upper.Clear();
    }

    ASSERT_OK(tablet()->CreateSubtablet(
        subtablet_id, *partition, key_bounds, yb::OpId() /* split_op_id */,
        HybridTime() /* split_hybrid_time */));
    split_tablets.push_back(ASSERT_RESULT(harness_->OpenTablet(subtablet_id)));

    partition->set_partition_key_start(partition->partition_key_end());
    key_bounds.lower = key_bounds.upper;
  }

  for (auto split_tablet : split_tablets) {
    const auto split_docdb_dump_str = split_tablet->TEST_DocDBDumpStr(IncludeIntents::kTrue);

    // Before compaction underlying DocDB dump should be the same.
    ASSERT_EQ(source_docdb_dump_str, split_docdb_dump_str);

    // But split tablets should only return relevant data without overlap and no unexpected data.
    const auto& split_partition = split_tablet->metadata()->partition();
    const auto start_hash = PartitionKeyToHash(split_partition->partition_key_start());
    const auto end_hash = PartitionKeyToHash(split_partition->partition_key_end());

    for (const auto& row : ASSERT_RESULT(SelectAll(split_tablet.get()))) {
      const auto hash_code = GetRowHashCode(row);
      if (start_hash) {
        ASSERT_GE(hash_code, *start_hash);
      }
      if (end_hash) {
        ASSERT_LT(hash_code, *end_hash);
      }
      ASSERT_EQ(source_rows.erase(row.ToString()), 1);
    }

    split_tablet->ForceRocksDBCompactInTest();

    VLOG(1) << split_tablet->tablet_id() << " compacted:" << std::endl
            << split_tablet->TEST_DocDBDumpStr(IncludeIntents::kTrue);

    // After compaction split tablets' RocksDB instances should have no overlap and no unexpected
    // data.
    std::unordered_set<std::string> split_docdb_dump;
    split_tablet->TEST_DocDBDumpToContainer(IncludeIntents::kTrue, &split_docdb_dump);
    for (const auto& entry : split_docdb_dump) {
      ASSERT_EQ(source_docdb_dump.erase(entry), 1);
    }

    // Check data returned by tablet.
    for (const auto& row : ASSERT_RESULT(SelectAll(split_tablet.get()))) {
      ASSERT_EQ(source_rows2.erase(row.ToString()), 1);
    }

    // Each split tablet data size should be less than original data size divided by number
    // of split points.
    ASSERT_LT(
        split_tablet->doc_db().regular->GetCurrentVersionDataSstFilesSize(),
        tablet()->doc_db().regular->GetCurrentVersionDataSstFilesSize() / kNumSplits);
  }

  // Split tablets should have all data from the source tablet.
  ASSERT_TRUE(source_rows.empty()) << boost::algorithm::join(source_rows, "\n");
  ASSERT_TRUE(source_rows2.empty()) << boost::algorithm::join(source_rows2, "\n");
  ASSERT_TRUE(source_docdb_dump.empty()) << boost::algorithm::join(source_docdb_dump, "\n");
}

// TODO: Need to test with distributed transactions both pending and committed
// (but not yet applied) during split.
// Split tablets should not return unexpected data for not yet applied, but committed transactions
// before and after compaction.
// Also check that non-relevant intents are cleaned from split intents DB after compaction.
//
// This test would be possible as an integration test when upper layers of tablet splitting are
// implemented.

} // namespace tablet
} // namespace yb
