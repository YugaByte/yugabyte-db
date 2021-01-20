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

#include <algorithm>
#include <vector>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "yb/common/ql_expr.h"
#include "yb/common/ql_protocol_util.h"
#include "yb/common/schema.h"

#include "yb/docdb/doc_rowwise_iterator.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet-test-base.h"
#include "yb/util/test_macros.h"
#include "yb/util/test_util.h"

namespace yb {
namespace tablet {

class TabletPushdownTest : public YBTabletTest {
 public:
  TabletPushdownTest()
    : YBTabletTest(Schema({ ColumnSchema("key", INT32, false, true),
                            ColumnSchema("int_val", INT32),
                            ColumnSchema("string_val", STRING) }, 1)) {
  }

  void SetUp() override {
    YBTabletTest::SetUp();

    FillTestTablet();
  }

  void FillTestTablet() {
    nrows_ = 2100;
    if (AllowSlowTests()) {
      nrows_ = 100000;
    }

    LocalTabletWriter writer(tablet().get());
    QLWriteRequestPB req;
    for (int64_t i = 0; i < nrows_; i++) {
      QLAddInt32HashValue(&req, i);
      QLAddInt32ColumnValue(&req, kFirstColumnId + 1, i * 10);
      QLAddStringColumnValue(&req, kFirstColumnId + 2, StringPrintf("%08" PRId64, i));
      ASSERT_OK_FAST(writer.Write(&req));
    }
  }

  // The predicates tested in the various test cases all yield
  // the same set of rows. Run the scan and verify that the
  // expected rows are returned.
  void TestScanYieldsExpectedResults(int column_id, int lower, int upper) {
    ReadHybridTime read_time = ReadHybridTime::SingleTime(CHECK_RESULT(tablet()->SafeTime()));
    QLReadRequestPB req;
    auto* condition = req.mutable_where_expr()->mutable_condition();
    condition->set_op(QLOperator::QL_OP_AND);
    QLAddInt32Condition(condition, column_id, QL_OP_GREATER_THAN_EQUAL, lower);
    QLAddInt32Condition(condition, column_id, QL_OP_LESS_THAN_EQUAL, upper);
    QLReadRequestResult result;
    TransactionMetadataPB transaction;
    QLAddColumns(schema_, {}, &req);
    EXPECT_OK(tablet()->HandleQLReadRequest(
        CoarseTimePoint::max() /* deadline */, read_time, req, transaction, &result));

    ASSERT_EQ(QLResponsePB::YQL_STATUS_OK, result.response.status())
        << "Error: " << result.response.error_message();

    auto row_block = CreateRowBlock(QLClient::YQL_CLIENT_CQL, schema_, result.rows_data);
    std::vector<std::string> results;
    for (const auto& row : row_block->rows()) {
      results.push_back(row.ToString());
    }
    std::sort(results.begin(), results.end());
    for (const string &str : results) {
      LOG(INFO) << str;
    }
    ASSERT_EQ(11, results.size());
    ASSERT_EQ("{ int32:200, int32:2000, string:\"00000200\" }", results[0]);
    ASSERT_EQ("{ int32:210, int32:2100, string:\"00000210\" }", results[10]);
  }

 private:
  uint64_t nrows_;
};

TEST_F(TabletPushdownTest, TestPushdownIntKeyRange) {
  TestScanYieldsExpectedResults(kFirstColumnId, 200, 210);
}

// TODO: Value range scan is not working yet, it returns 2100 rows.
TEST_F(TabletPushdownTest, TestPushdownIntValueRange) {
  // Push down a double-ended range on the integer value column.
  TestScanYieldsExpectedResults(kFirstColumnId + 1, 2000, 2100);
}

} // namespace tablet
} // namespace yb
