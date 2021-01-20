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

#include "yb/client/ql-dml-test-base.h"
#include "yb/client/session.h"
#include "yb/client/table_handle.h"

#include "yb/common/ql_value.h"

#include "yb/yql/cql/ql/util/statement_result.h"

#include "yb/util/random_util.h"

using namespace std::literals;

namespace yb {
namespace client {

namespace {

constexpr int kCollectionSize = 10;

std::string To8LengthHex(uint32_t value) {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%08x", value);
  return buffer;
}

template <class RequestPB>
void AddHash(int32_t hash_seed, RequestPB* req) {
  QLAddInt32HashValue(req, hash_seed);
  QLAddInt32HashValue(req, ~hash_seed);
  auto s1 = To8LengthHex(hash_seed);
  auto s2 = To8LengthHex(~hash_seed);
  // Need 40 chars long string.
  QLAddStringHashValue(req, s1 + s2 + s1 + s2 + s1);
}

// Returns seed for appropriate row. Seed is used to generate all row values.
int32_t RowSeed(int32_t hash_seed, int32_t range) {
  return (hash_seed << 16) + range;
}

int32_t ListEntry(int32_t hash_seed, int32_t range, int32_t i) {
  return RowSeed(hash_seed, range) * i;
}

} // namespace

class QLListTest : public QLDmlTestBase<MiniCluster> {
 public:
  QLListTest() {
  }

  void SetUp() override {
    QLDmlTestBase::SetUp();

    YBSchemaBuilder b;
    b.AddColumn("h1")->Type(INT32)->HashPrimaryKey()->NotNull();
    b.AddColumn("h2")->Type(INT32)->HashPrimaryKey()->NotNull();
    b.AddColumn("h3")->Type(STRING)->HashPrimaryKey()->NotNull();
    b.AddColumn("r1")->Type(INT32)->PrimaryKey()->NotNull();
    b.AddColumn("l1")->Type(QLType::CreateTypeList(DataType::INT32));
    b.AddColumn("s1")->Type(QLType::CreateTypeSet(DataType::STRING));
    b.AddColumn("s2")->Type(QLType::CreateTypeSet(DataType::STRING));

    ASSERT_OK(table_.Create(kTableName, CalcNumTablets(3), client_.get(), &b));
  }

  void InsertRows(YBSession* session, int32_t hash_seed, int32_t ranges) {
    std::vector<YBOperationPtr> ops;
    for (int32_t range = 0; range != ranges; ++range) {
      auto op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
      auto* const req = op->mutable_request();
      AddHash(hash_seed, req);
      QLAddInt32RangeValue(req, range);
      auto l1 = table_.PrepareColumn(req, "l1")->mutable_list_value();
      auto s1 = table_.PrepareColumn(req, "s1")->mutable_set_value();
      auto s2 = table_.PrepareColumn(req, "s2")->mutable_set_value();
      int32_t seed = RowSeed(hash_seed, range);
      for (int i = 1; i <= kCollectionSize; ++i) {
        l1->add_elems()->set_int32_value(ListEntry(hash_seed, range, i));
        s1->add_elems()->set_string_value(To8LengthHex(seed * i));
        s2->add_elems()->set_string_value(To8LengthHex((~seed) * i));
      }
      ops.push_back(std::move(op));
    }
    ASSERT_OK(session->ApplyAndFlush(ops));
  }

  std::unique_ptr<QLRowBlock> ReadRows(YBSession* session, int32_t hash_seed) {
    auto op = table_.NewReadOp();
    auto* const req = op->mutable_request();
    AddHash(hash_seed, req);
    table_.AddColumns(table_.AllColumnNames(), req);
    EXPECT_OK(session->ApplyAndFlush(op));
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);

    return ql::RowsResult(op.get()).GetRowBlock();
  }

  TableHandle table_;
};

TEST_F(QLListTest, Simple) {
  auto session = NewSession();
  constexpr int kKeys = 2;
  constexpr int kRanges = 10;
  for (int i = 1; i <= kKeys; ++i) {
    InsertRows(session.get(), i, kRanges);
  }

  for (int k = 1; k <= kKeys; ++k) {
    auto rowblock = ReadRows(session.get(), k);
    ASSERT_EQ(kRanges, rowblock->row_count());
    for (int r = 0; r != kRanges; ++r) {
      const auto& row = rowblock->row(r);
      ASSERT_EQ(r, row.column(3).int32_value());
      const auto& l1 = row.column(4).list_value();
      ASSERT_EQ(kCollectionSize, l1.elems_size());
      for (int i = 1; i <= kCollectionSize; ++i) {
        SCOPED_TRACE(Format("k: $0, r: $1, i: $2", k, r, i));
        ASSERT_EQ(ListEntry(k, r, i), l1.elems(i - 1).int32_value());
      }
      // TODO add string set verification
    }
  }
}

TEST_F(QLListTest, Performance) {
  DontVerifyClusterBeforeNextTearDown(); // To remove checksum from perf report

  std::atomic<bool> stop(false);
  std::atomic<int> inserted(0);
  std::atomic<int> total_reads(0);
  constexpr int kRanges = 10;
  constexpr int kReaders = 4;
  constexpr int kWarmupInserts = 100;
  const auto kRunTime = 15s;

  std::thread writer([this, &stop, &inserted] {
    auto session = NewSession();
    while (!stop.load()) {
      auto index = ++inserted;
      InsertRows(session.get(), index, kRanges);
      if (index >= kWarmupInserts) {
        std::this_thread::sleep_for(10ms);
      }
    }
  });

  while (inserted.load() < kWarmupInserts) {
    std::this_thread::sleep_for(10ms);
  }

  std::vector<std::thread> readers;
  for (int i = 0; i != kReaders; ++i) {
    readers.emplace_back([this, &stop, &inserted, &total_reads, kRanges] {
      auto session = NewSession();
      while (!stop.load()) {
        auto hash_seed = RandomUniformInt(1, inserted.load() - 1);
        auto rowblock = ReadRows(session.get(), hash_seed);
        ASSERT_EQ(kRanges, rowblock->row_count()) << "Seed: " << hash_seed;
        ++total_reads;
      }
    });
  }

  std::this_thread::sleep_for(kRunTime);

  stop = true;
  LOG(INFO) << "Total writes: " << inserted.load() << ", total reads: " << total_reads.load();

  writer.join();
  for (auto& t : readers) {
    t.join();
  }
}

}  // namespace client
}  // namespace yb
