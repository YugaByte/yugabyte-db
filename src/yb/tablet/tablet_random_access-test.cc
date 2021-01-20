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

#include <string>
#include <vector>

#include <glog/logging.h>
#include <glog/stl_logging.h>
#include <gtest/gtest.h>
#include <gflags/gflags.h>

#include "yb/common/ql_expr.h"
#include "yb/common/schema.h"

#include "yb/gutil/casts.h"
#include "yb/gutil/strings/join.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet-test-base.h"
#include "yb/util/stopwatch.h"

DEFINE_int32(keyspace_size, 300, "number of unique row keys to insert/mutate");
DEFINE_int32(runtime_seconds, 1, "number of seconds to run the test");
DEFINE_int32(sleep_between_background_ops_ms, 100,
             "number of milliseconds to sleep between flushing or compacting");
DEFINE_int32(update_delete_ratio, 4, "ratio of update:delete when mutating existing rows");

using std::string;
using std::vector;

enum TestOp {
  TEST_INSERT,
  TEST_UPDATE,
  TEST_DELETE,
  TEST_FLUSH_OPS,
  TEST_FLUSH_TABLET,
  TEST_NUM_OP_TYPES // max value for enum
};
MAKE_ENUM_LIMITS(TestOp, TEST_INSERT, TEST_NUM_OP_TYPES);

namespace yb {
namespace tablet {

const char* TestOp_names[] = {
  "TEST_INSERT",
  "TEST_UPDATE",
  "TEST_DELETE",
  "TEST_FLUSH_OPS",
  "TEST_FLUSH_TABLET",
};

// Test which does only random operations against a tablet, including update and random
// get (ie scans with equal lower and upper bounds).
//
// The test maintains an in-memory copy of the expected state of the tablet, and uses only
// a single thread, so that it's easy to verify that the tablet always matches the expected
// state.
class TestRandomAccess : public YBTabletTest {
  static constexpr auto VALUE_NOT_FOUND = "";

 public:
  TestRandomAccess()
    : YBTabletTest(
        Schema({ ColumnSchema("key", INT32, false, true), ColumnSchema("val", INT32, true) }, 1)) {
    OverrideFlagForSlowTests("keyspace_size", "30000");
    OverrideFlagForSlowTests("runtime_seconds", "10");
    OverrideFlagForSlowTests("sleep_between_background_ops_ms", "1000");

    expected_tablet_state_.resize(FLAGS_keyspace_size);
  }

  void SetUp() override {
    YBTabletTest::SetUp();
    writer_.reset(new LocalTabletWriter(tablet().get()));
  }

  // Pick a random row of the table, verify its current state, and then
  // modify it in some way. The modifications may include multiple mutations
  // to the same row in a single batch (eg insert/update/delete).
  //
  // The mutations are always valid. For example:
  // - inserting if it doesn't exist yet
  // - perform an update or delete the row if it does exist.
  //
  // TODO: should add a version of this test which also tries invalid operations
  // and validates the correct errors.
  void DoRandomBatch() {
    if (expected_tablet_state_.size() == 0)
      return;
    int key = rand_r(&random_seed_) % expected_tablet_state_.size();
    string& cur_val = expected_tablet_state_[key];

    // Check that a read yields what we expect.
    string val_in_table = GetRow(key);
    // Since we start with expected_tablet_state_ sized `keyspace_size`, there might not
    // be all keys present initially. So we do not assert for the value when key is not present.
    ASSERT_EQ(cur_val, val_in_table);

    LocalTabletWriter::Batch pending;
    for (int i = 0; i < 3; i++) {
      int new_val = rand_r(&random_seed_);
      if (cur_val.empty()) {
        // If there is no row, then insert one.
        cur_val = InsertRow(key, new_val, &pending);
      } else {
        if (new_val % (FLAGS_update_delete_ratio + 1) == 0) {
          cur_val = DeleteRow(key, &pending);
        } else {
          cur_val = MutateRow(key, new_val, &pending);
        }
      }
    }
    CHECK_OK(writer_->WriteBatch(&pending));
  }

  void DoRandomBatches() {
    int op_count = 0;
    Stopwatch s;
    s.start();
    while (s.elapsed().wall_seconds() < FLAGS_runtime_seconds) {
      for (int i = 0; i < 100; i++) {
        ASSERT_NO_FATALS(DoRandomBatch());
        op_count++;
      }
    }
    LOG(INFO) << "Ran " << op_count << " ops "
              << "(" << (op_count / s.elapsed().wall_seconds()) << " ops/sec)";
  }

  // Wakes up periodically to perform a flush or compaction.
  void BackgroundOpThread() {
    while (!done_.WaitFor(MonoDelta::FromMilliseconds(FLAGS_sleep_between_background_ops_ms))) {
      CHECK_OK(tablet()->Flush(tablet::FlushMode::kSync));
    }
  }

  std::string UpsertRow(QLWriteRequestPB::QLStmtType stmt, int key, int val,
                        LocalTabletWriter::Batch* batch) {
    QLWriteRequestPB* req = batch->Add();
    req->set_type(stmt);
    QLAddInt32HashValue(req, key);
    if (val & 1) {
      QLAddNullColumnValue(req, kFirstColumnId + 1);
    } else {
      QLAddInt32ColumnValue(req, kFirstColumnId + 1, val);
    }
    return Format("{ int32:$0, $1 }", key, val & 1 ? "null" : Format("int32:$0", val));
  }

  // Adds an insert for the given key/value pair to 'ops', returning the new stringified
  // value of the row.
  string InsertRow(int key, int val, LocalTabletWriter::Batch* batch) {
    return UpsertRow(QLWriteRequestPB::QL_STMT_INSERT, key, val, batch);
  }

  // Adds an update of the given key/value pair to 'ops', returning the new stringified
  // value of the row.
  string MutateRow(int key, uint32_t new_val, LocalTabletWriter::Batch* batch) {
    return UpsertRow(QLWriteRequestPB::QL_STMT_UPDATE, key, new_val, batch);
  }

  // Adds a delete of the given row to 'ops', returning an empty string (indicating that
  // the row no longer exists).
  std::string DeleteRow(int key, LocalTabletWriter::Batch* batch) {
    auto req = batch->Add();
    req->set_type(QLWriteRequestPB::QL_STMT_DELETE);
    QLAddInt32HashValue(req, key);
    return std::string();
  }

  // Random-read the given row, returning its current value.
  // If the row doesn't exist, returns "()".
  string GetRow(int key) {
    ReadHybridTime read_time = ReadHybridTime::SingleTime(CHECK_RESULT(tablet()->SafeTime()));
    QLReadRequestPB req;
    auto* condition = req.mutable_where_expr()->mutable_condition();
    QLSetInt32Condition(condition, kFirstColumnId, QL_OP_EQUAL, key);
    QLReadRequestResult result;
    TransactionMetadataPB transaction;
    QLAddColumns(schema_, {}, &req);
    EXPECT_OK(tablet()->HandleQLReadRequest(
        CoarseTimePoint::max() /* deadline */, read_time, req, transaction, &result));

    EXPECT_EQ(QLResponsePB::YQL_STATUS_OK, result.response.status());

    auto row_block = CreateRowBlock(QLClient::YQL_CLIENT_CQL, schema_, result.rows_data);
    if (row_block->row_count() == 0) {
      return VALUE_NOT_FOUND;
    }
    EXPECT_EQ(1, row_block->row_count());
    return row_block->row(0).ToString();
  }

 protected:
  void RunFuzzCase(const vector<TestOp>& ops,
                   int update_multiplier);

  // The current expected state of the tablet.
  vector<string> expected_tablet_state_;

  // Latch triggered when the main thread is finished performing
  // operations. This stops the compact/flush thread.
  CountDownLatch done_{1};

  gscoped_ptr<LocalTabletWriter> writer_;

  unsigned int random_seed_ = SeedRandom();
};

TEST_F(TestRandomAccess, Test) {
  scoped_refptr<Thread> flush_thread;
  CHECK_OK(Thread::Create(
      "test", "flush", std::bind(&TestRandomAccess::BackgroundOpThread, this), &flush_thread));

  DoRandomBatches();
  done_.CountDown();
  flush_thread->Join();
}

void GenerateTestCase(vector<TestOp>* ops, int len) {
  bool exists = false;
  bool ops_pending = false;
  ops->clear();
  unsigned int random_seed = SeedRandom();
  while (ops->size() < len) {
    TestOp r = tight_enum_cast<TestOp>(
        rand_r(&random_seed) % enum_limits<TestOp>::max_enumerator);
    switch (r) {
      case TEST_INSERT:
        if (exists) continue;
        ops->push_back(TEST_INSERT);
        ops_pending = true;
        exists = true;
        break;
      case TEST_UPDATE:
        if (!exists) continue;
        ops->push_back(TEST_UPDATE);
        ops_pending = true;
        break;
      case TEST_DELETE:
        if (!exists) continue;
        ops->push_back(TEST_DELETE);
        exists = false;
        break;
      case TEST_FLUSH_OPS:
        if (ops_pending) {
          ops->push_back(TEST_FLUSH_OPS);
          ops_pending = false;
        }
        break;
      case TEST_FLUSH_TABLET:
        ops->push_back(TEST_FLUSH_TABLET);
        break;
      default:
        LOG(FATAL);
    }
  }
}

string DumpTestCase(const vector<TestOp>& ops) {
  vector<string> names;
  for (TestOp test_op : ops) {
    names.push_back(TestOp_names[test_op]);
  }
  return JoinStrings(names, ",\n");
}

void TestRandomAccess::RunFuzzCase(const vector<TestOp>& test_ops,
                                   int update_multiplier = 1) {
  LOG(INFO) << "test case: " << DumpTestCase(test_ops);

  LocalTabletWriter writer(tablet().get());
  LocalTabletWriter::Batch batch;

  string cur_val = "";
  string pending_val = "";

  int i = 0;
  for (TestOp test_op : test_ops) {
    string val_in_table = GetRow(1);
    if (val_in_table != VALUE_NOT_FOUND) {
      ASSERT_EQ(cur_val, val_in_table);
    }

    i++;
    LOG(INFO) << TestOp_names[test_op];
    switch (test_op) {
      case TEST_INSERT:
        pending_val = InsertRow(1, i, &batch);
        break;
      case TEST_UPDATE:
        for (int j = 0; j < update_multiplier; j++) {
          pending_val = MutateRow(1, i, &batch);
        }
        break;
      case TEST_DELETE:
        pending_val = DeleteRow(1, &batch);
        break;
      case TEST_FLUSH_OPS:
        ASSERT_OK(writer.WriteBatch(&batch));
        cur_val = pending_val;
        break;
      case TEST_FLUSH_TABLET:
        ASSERT_OK(tablet()->Flush(tablet::FlushMode::kSync));
        break;
      default:
        LOG(FATAL) << test_op;
    }
  }
}

// Generates a random test sequence and runs it.
// The logs of this test are designed to easily be copy-pasted and create
// more specific test cases like TestFuzz<N> below.
TEST_F(TestRandomAccess, TestFuzz) {
  SeedRandom();
  vector<TestOp> test_ops;
  GenerateTestCase(&test_ops, 500);
  RunFuzzCase(test_ops);
}

// Generates a random test case, but the UPDATEs are all repeated 1000 times.
// This results in very large batches which are likely to span multiple delta blocks
// when flushed.
TEST_F(TestRandomAccess, TestFuzzHugeBatches) {
  SeedRandom();
  vector<TestOp> test_ops;
  GenerateTestCase(&test_ops, AllowSlowTests() ? 1000 : 50);
  RunFuzzCase(test_ops, 1000);
}

// A particular test case which previously failed TestFuzz.
TEST_F(TestRandomAccess, TestFuzz1) {
  TestOp test_ops[] = {
    // Get an inserted row.
    TEST_INSERT,
    TEST_FLUSH_OPS,
    TEST_FLUSH_TABLET,
    // DELETE and INSERT.
    TEST_DELETE,
    TEST_INSERT,
    TEST_FLUSH_OPS,
    TEST_FLUSH_TABLET,
  };
  RunFuzzCase(vector<TestOp>(test_ops, test_ops + arraysize(test_ops)));
}

// A particular test case which previously failed TestFuzz.
TEST_F(TestRandomAccess, TestFuzz2) {
  TestOp test_ops[] = {
    TEST_INSERT,
    TEST_DELETE,
    TEST_FLUSH_OPS,
    TEST_FLUSH_TABLET,
    TEST_INSERT,
    TEST_DELETE,
    TEST_INSERT,
    TEST_FLUSH_OPS,
    TEST_FLUSH_TABLET,
    TEST_DELETE,
  };
  RunFuzzCase(vector<TestOp>(test_ops, test_ops + arraysize(test_ops)));
}

// A particular test case which previously failed TestFuzz.
TEST_F(TestRandomAccess, TestFuzz3) {
  TestOp test_ops[] = {
    TEST_INSERT,
    TEST_DELETE,
    TEST_FLUSH_OPS,
    TEST_FLUSH_TABLET,
    TEST_INSERT,
    TEST_DELETE,
  };
  RunFuzzCase(vector<TestOp>(test_ops, test_ops + arraysize(test_ops)));
}

// A particular test case which previously failed TestFuzz.
TEST_F(TestRandomAccess, TestFuzz4) {
  TestOp test_ops[] = {
    TEST_INSERT,
    TEST_FLUSH_OPS,
    TEST_FLUSH_TABLET,
    TEST_DELETE,
    TEST_INSERT,
    TEST_UPDATE,
    TEST_DELETE,
    TEST_FLUSH_OPS,
    TEST_FLUSH_TABLET,
    TEST_INSERT,
    TEST_UPDATE,
    TEST_UPDATE,
    TEST_DELETE,
    TEST_INSERT,
    TEST_DELETE,
    TEST_FLUSH_OPS,
    TEST_FLUSH_TABLET,
  };
  RunFuzzCase(vector<TestOp>(test_ops, test_ops + arraysize(test_ops)));
}

} // namespace tablet
} // namespace yb
