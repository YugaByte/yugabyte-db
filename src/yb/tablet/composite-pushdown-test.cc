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

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "yb/common/ql_expr.h"
#include "yb/common/schema.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet-test-base.h"
#include "yb/util/test_macros.h"
#include "yb/util/test_util.h"

namespace yb {
namespace tablet {

const char* const kTestHostnames[] = { "foo", "foobar", "baz", nullptr };

class CompositePushdownTest : public YBTabletTest {
 public:
  CompositePushdownTest()
      : YBTabletTest(Schema({ ColumnSchema("year", INT16, false, true),
                              ColumnSchema("month", INT8, false, true),
                              ColumnSchema("day", INT8, false, true),
                              ColumnSchema("hostname", STRING, false, true),
                              ColumnSchema("data", STRING) },
                            4)) {
  }

  void SetUp() override {
    YBTabletTest::SetUp();

    FillTestTablet();
  }

  void FillTestTablet() {
    uint32_t nrows = 10 * 12 * 28;
    int i = 0;

    LocalTabletWriter writer(tablet().get());
    for (int16_t year = 2000; year <= 2010; year++) {
      for (int8_t month = 1; month <= 12; month++) {
        for (int8_t day = 1; day <= 28; day++) {
          for (int host_idx = 0; kTestHostnames[host_idx] != nullptr; host_idx++) {
            QLWriteRequestPB req;
            QLAddInt16HashValue(&req, year);
            QLAddInt8HashValue(&req, month);
            QLAddInt8HashValue(&req, day);
            QLAddStringHashValue(&req, kTestHostnames[host_idx]);
            QLAddStringColumnValue(&req, kFirstColumnId + 4,
                StringPrintf("%d/%02d/%02d-%s", year, month, day, kTestHostnames[host_idx]));
            ASSERT_OK_FAST(writer.Write(&req));

            if (i == nrows * 9 / 10) {
              ASSERT_OK(tablet()->Flush(tablet::FlushMode::kSync));
            }
            ++i;
          }
        }
      }
    }
  }

  // Helper function for sorting returned results by the 'data' field.
  // This is needed as "2" is lexicographically greater than "12" which means
  // that, e.g., comparing "(int16 year=2001, int8 month=2, int8 day=7, string
  // data=2001/02/07)" to "(int16 year=2001, int8 month=12, int8
  // day=7, string data=2001/12/07)" would be semantically incorrect if
  // the comparison was on the whole string vs the last portion of the
  // string ("2001/02/01" vs. "2001/12/01")
  struct SuffixComparator {
    bool operator()(const string &a, const string &b) {
      string s_a = a.substr(a.find("\", string:"));
      string s_b = b.substr(b.find("\", string:"));
      return s_a < s_b;
    }
  };

  void ScanTablet(QLReadRequestPB* req, vector<string> *results) {
    ReadHybridTime read_time = ReadHybridTime::SingleTime(ASSERT_RESULT(tablet()->SafeTime()));
    QLReadRequestResult result;
    TransactionMetadataPB transaction;
    QLAddColumns(schema_, {}, req);
    EXPECT_OK(tablet()->HandleQLReadRequest(
        CoarseTimePoint::max() /* deadline */, read_time, *req, transaction, &result));

    ASSERT_EQ(QLResponsePB::YQL_STATUS_OK, result.response.status())
        << "Error: " << result.response.error_message();

    auto row_block = CreateRowBlock(QLClient::YQL_CLIENT_CQL, schema_, result.rows_data);
    for (const auto& row : row_block->rows()) {
      results->push_back(row.ToString());
    }
    std::sort(results->begin(), results->end(), SuffixComparator());
    for (const string &str : *results) {
      VLOG(1) << str;
    }
  }
};

TEST_F(CompositePushdownTest, TestPushDownExactEquality) {
  int16_t year = 2001;
  int8_t month = 9;
  int8_t day = 7;

  QLReadRequestPB req;
  auto cond = req.mutable_where_expr()->mutable_condition();
  cond->set_op(QLOperator::QL_OP_AND);
  QLAddInt16Condition(cond, kFirstColumnId, QL_OP_EQUAL, year);
  cond = cond->add_operands()->mutable_condition();
  cond->set_op(QLOperator::QL_OP_AND);
  QLAddInt8Condition(cond, kFirstColumnId + 1, QL_OP_EQUAL, month);
  cond = cond->add_operands()->mutable_condition();
  cond->set_op(QLOperator::QL_OP_AND);
  QLAddInt8Condition(cond, kFirstColumnId + 2, QL_OP_EQUAL, day);
  QLAddStringCondition(cond, kFirstColumnId + 3, QL_OP_EQUAL, kTestHostnames[0]);

  vector<string> results;
  ASSERT_NO_FATALS(ScanTablet(&req, &results));
  ASSERT_EQ(1, results.size());
  EXPECT_EQ("{ int16:2001, int8:9, int8:7, string:\"foo\", string:\"2001/09/07-foo\" }",
            results.front());
}


// Test for "host <= 'foo'" which should reject 'foobaz'.
// Regression test for a bug in an earlier implementation of predicate pushdown.
TEST_F(CompositePushdownTest, TestPushDownStringInequality) {
  int16_t year = 2001;
  int8_t month = 9;
  int8_t day = 7;

  QLReadRequestPB req;
  auto cond = req.mutable_where_expr()->mutable_condition();
  cond->set_op(QLOperator::QL_OP_AND);
  QLAddInt16Condition(cond, kFirstColumnId, QL_OP_EQUAL, year);
  cond = cond->add_operands()->mutable_condition();
  cond->set_op(QLOperator::QL_OP_AND);
  QLAddInt8Condition(cond, kFirstColumnId + 1, QL_OP_EQUAL, month);
  cond = cond->add_operands()->mutable_condition();
  cond->set_op(QLOperator::QL_OP_AND);
  QLAddInt8Condition(cond, kFirstColumnId + 2, QL_OP_EQUAL, day);
  QLAddStringCondition(cond, kFirstColumnId + 3, QL_OP_LESS_THAN_EQUAL, kTestHostnames[0]);

  vector<string> results;
  ASSERT_NO_FATALS(ScanTablet(&req, &results));
  ASSERT_EQ(2, results.size());
  EXPECT_EQ("{ int16:2001, int8:9, int8:7, string:\"baz\", string:\"2001/09/07-baz\" }",
            results.front());
  EXPECT_EQ("{ int16:2001, int8:9, int8:7, string:\"foo\", string:\"2001/09/07-foo\" }",
            results.back());
}

TEST_F(CompositePushdownTest, TestPushDownDateEquality) {
  int16_t year = 2001;
  int8_t month = 9;
  int8_t day = 7;

  QLReadRequestPB req;
  auto cond = req.mutable_where_expr()->mutable_condition();
  cond->set_op(QLOperator::QL_OP_AND);
  QLAddInt16Condition(cond, kFirstColumnId, QL_OP_EQUAL, year);
  cond = cond->add_operands()->mutable_condition();
  cond->set_op(QLOperator::QL_OP_AND);
  QLAddInt8Condition(cond, kFirstColumnId + 1, QL_OP_EQUAL, month);
  QLAddInt8Condition(cond, kFirstColumnId + 2, QL_OP_EQUAL, day);

  vector<string> results;
  ASSERT_NO_FATALS(ScanTablet(&req, &results));
  ASSERT_EQ(3, results.size());
  EXPECT_EQ("{ int16:2001, int8:9, int8:7, string:\"baz\", string:\"2001/09/07-baz\" }",
            results[0]);
  EXPECT_EQ("{ int16:2001, int8:9, int8:7, string:\"foo\", string:\"2001/09/07-foo\" }",
            results[1]);
  EXPECT_EQ("{ int16:2001, int8:9, int8:7, string:\"foobar\", string:\"2001/09/07-foobar\" }",
            results[2]);
}

TEST_F(CompositePushdownTest, TestPushDownPrefixEquality) {
  int16_t year = 2001;
  int8_t month = 9;

  {
    QLReadRequestPB req;
    auto cond = req.mutable_where_expr()->mutable_condition();
    cond->set_op(QLOperator::QL_OP_AND);
    QLAddInt16Condition(cond, kFirstColumnId, QL_OP_EQUAL, year);
    QLAddInt8Condition(cond, kFirstColumnId + 1, QL_OP_EQUAL, month);

    vector<string> results;
    ASSERT_NO_FATALS(ScanTablet(&req, &results));
    ASSERT_EQ(28 * 3, results.size());
    EXPECT_EQ("{ int16:2001, int8:9, int8:1, string:\"baz\", string:\"2001/09/01-baz\" }",
              results.front());
    EXPECT_EQ("{ int16:2001, int8:9, int8:28, string:\"foobar\", string:\"2001/09/28-foobar\" }",
              results.back());
  }

  {
    QLReadRequestPB req;
    auto cond = req.mutable_where_expr()->mutable_condition();
    QLSetInt16Condition(cond, kFirstColumnId, QL_OP_EQUAL, year);

    vector<string> results;
    ASSERT_NO_FATALS(ScanTablet(&req, &results));
    ASSERT_EQ(28 * 12 * 3, results.size());
    EXPECT_EQ("{ int16:2001, int8:1, int8:1, string:\"baz\", string:\"2001/01/01-baz\" }",
              results.front());
    EXPECT_EQ("{ int16:2001, int8:2, int8:1, string:\"baz\", string:\"2001/02/01-baz\" }",
              results[28 * 3]);
    EXPECT_EQ("{ int16:2001, int8:12, int8:28, string:\"foobar\", string:\"2001/12/28-foobar\" }",
              results.back());
  }
}

TEST_F(CompositePushdownTest, TestPushDownPrefixEqualitySuffixInequality) {
  int16_t year = 2001;
  int8_t month_l = 9;
  int8_t month_u = 11;
  int8_t day_l = 1;
  int8_t day_u = 15;

  {
    // year=2001, month=9, day >= 1 && day <= 15
    QLReadRequestPB req;
    auto cond = req.mutable_where_expr()->mutable_condition();
    cond->set_op(QLOperator::QL_OP_AND);
    QLAddInt16Condition(cond, kFirstColumnId, QL_OP_EQUAL, year);
    cond = cond->add_operands()->mutable_condition();
    cond->set_op(QLOperator::QL_OP_AND);
    QLAddInt8Condition(cond, kFirstColumnId + 1, QL_OP_EQUAL, month_l);
    cond = cond->add_operands()->mutable_condition();
    cond->set_op(QLOperator::QL_OP_AND);
    QLAddInt8Condition(cond, kFirstColumnId + 2, QL_OP_GREATER_THAN_EQUAL, day_l);
    QLAddInt8Condition(cond, kFirstColumnId + 2, QL_OP_LESS_THAN_EQUAL, day_u);

    vector<string> results;
    ASSERT_NO_FATALS(ScanTablet(&req, &results));
    ASSERT_EQ(15 * 3, results.size());
    EXPECT_EQ("{ int16:2001, int8:9, int8:1, string:\"baz\", string:\"2001/09/01-baz\" }",
              results.front());
    EXPECT_EQ("{ int16:2001, int8:9, int8:15, string:\"foobar\", string:\"2001/09/15-foobar\" }",
              results.back());
  }

  {
    // year=2001, month=9, day >= 1
    QLReadRequestPB req;
    auto cond = req.mutable_where_expr()->mutable_condition();
    cond->set_op(QLOperator::QL_OP_AND);
    QLAddInt16Condition(cond, kFirstColumnId, QL_OP_EQUAL, year);
    cond = cond->add_operands()->mutable_condition();
    cond->set_op(QLOperator::QL_OP_AND);
    QLAddInt8Condition(cond, kFirstColumnId + 1, QL_OP_EQUAL, month_l);
    QLAddInt8Condition(cond, kFirstColumnId + 2, QL_OP_GREATER_THAN_EQUAL, day_l);

    vector<string> results;
    ASSERT_NO_FATALS(ScanTablet(&req, &results));
    ASSERT_EQ(28 * 3, results.size());
    EXPECT_EQ("{ int16:2001, int8:9, int8:1, string:\"baz\", string:\"2001/09/01-baz\" }",
              results.front());
    EXPECT_EQ("{ int16:2001, int8:9, int8:28, string:\"foobar\", string:\"2001/09/28-foobar\" }",
              results.back());
  }

  {
    // year=2001, month=9, day <= 15
    QLReadRequestPB req;
    auto cond = req.mutable_where_expr()->mutable_condition();
    cond->set_op(QLOperator::QL_OP_AND);
    QLAddInt16Condition(cond, kFirstColumnId, QL_OP_EQUAL, year);
    cond = cond->add_operands()->mutable_condition();
    cond->set_op(QLOperator::QL_OP_AND);
    QLAddInt8Condition(cond, kFirstColumnId + 1, QL_OP_EQUAL, month_l);
    QLAddInt8Condition(cond, kFirstColumnId + 2, QL_OP_LESS_THAN_EQUAL, day_u);

    vector<string> results;
    ASSERT_NO_FATALS(ScanTablet(&req, &results));
    ASSERT_EQ(15 * 3, results.size());
    EXPECT_EQ("{ int16:2001, int8:9, int8:1, string:\"baz\", string:\"2001/09/01-baz\" }",
              results.front());
    EXPECT_EQ("{ int16:2001, int8:9, int8:15, string:\"foobar\", string:\"2001/09/15-foobar\" }",
              results.back());
  }

  {
    // year=2001, month >= 9 && month <= 11
    QLReadRequestPB req;
    auto cond = req.mutable_where_expr()->mutable_condition();
    cond->set_op(QLOperator::QL_OP_AND);
    QLAddInt16Condition(cond, kFirstColumnId, QL_OP_EQUAL, year);
    cond = cond->add_operands()->mutable_condition();
    cond->set_op(QLOperator::QL_OP_AND);
    QLAddInt8Condition(cond, kFirstColumnId + 1, QL_OP_GREATER_THAN_EQUAL, month_l);
    QLAddInt8Condition(cond, kFirstColumnId + 1, QL_OP_LESS_THAN_EQUAL, month_u);

    vector<string> results;
    ASSERT_NO_FATALS(ScanTablet(&req, &results));
    ASSERT_EQ(3 * 28 * 3, results.size());
    EXPECT_EQ("{ int16:2001, int8:9, int8:1, string:\"baz\", string:\"2001/09/01-baz\" }",
              results.front());
    EXPECT_EQ( "{ int16:2001, int8:11, int8:28, string:\"foobar\", string:\"2001/11/28-foobar\" }",
              results.back());
  }

  {
    // year=2001, month <= 9
    QLReadRequestPB req;
    auto cond = req.mutable_where_expr()->mutable_condition();
    cond->set_op(QLOperator::QL_OP_AND);
    QLAddInt16Condition(cond, kFirstColumnId, QL_OP_EQUAL, year);
    QLAddInt8Condition(cond, kFirstColumnId + 1, QL_OP_LESS_THAN_EQUAL, month_l);

    vector<string> results;
    ASSERT_NO_FATALS(ScanTablet(&req, &results));
    ASSERT_EQ(9 * 28 * 3, results.size());
    EXPECT_EQ("{ int16:2001, int8:1, int8:1, string:\"baz\", string:\"2001/01/01-baz\" }",
               results.front());
    EXPECT_EQ("{ int16:2001, int8:9, int8:28, string:\"foobar\", string:\"2001/09/28-foobar\" }",
              results.back());
  }
}

TEST_F(CompositePushdownTest, TestPushdownPrefixInequality) {
  int16_t year_2001 = 2001;
  int16_t year_2003 = 2003;
  {
    // year >= 2001 && year <= 2003
    QLReadRequestPB req;
    auto cond = req.mutable_where_expr()->mutable_condition();
    cond->set_op(QLOperator::QL_OP_AND);
    QLAddInt16Condition(cond, kFirstColumnId, QL_OP_GREATER_THAN_EQUAL, year_2001);
    QLAddInt16Condition(cond, kFirstColumnId, QL_OP_LESS_THAN_EQUAL, year_2003);

    vector<string> results;
    ASSERT_NO_FATALS(ScanTablet(&req, &results));
    ASSERT_EQ(3 * 12 * 28 * 3, results.size());
    EXPECT_EQ("{ int16:2001, int8:1, int8:1, string:\"baz\", string:\"2001/01/01-baz\" }",
              results.front());
    EXPECT_EQ("{ int16:2003, int8:12, int8:28, string:\"foobar\", string:\"2003/12/28-foobar\" }",
              results.back());
  }

  {
    // year >= 2001
    QLReadRequestPB req;
    auto cond = req.mutable_where_expr()->mutable_condition();
    QLSetInt16Condition(cond, kFirstColumnId, QL_OP_GREATER_THAN_EQUAL, year_2001);

    vector<string> results;
    ASSERT_NO_FATALS(ScanTablet(&req, &results));
    ASSERT_EQ(10 * 12 * 28 * 3, results.size());
    EXPECT_EQ("{ int16:2001, int8:1, int8:1, string:\"baz\", string:\"2001/01/01-baz\" }",
              results.front());
    EXPECT_EQ("{ int16:2010, int8:12, int8:28, string:\"foobar\", string:\"2010/12/28-foobar\" }",
              results.back());
  }

  {
    // year <= 2003
    QLReadRequestPB req;
    auto cond = req.mutable_where_expr()->mutable_condition();
    QLSetInt16Condition(cond, kFirstColumnId, QL_OP_LESS_THAN_EQUAL, year_2003);

    vector<string> results;
    ASSERT_NO_FATALS(ScanTablet(&req, &results));
    ASSERT_EQ(4 * 12 * 28 * 3, results.size());
    EXPECT_EQ("{ int16:2000, int8:1, int8:1, string:\"baz\", string:\"2000/01/01-baz\" }",
              results.front());
    EXPECT_EQ("{ int16:2003, int8:12, int8:28, string:\"foobar\", string:\"2003/12/28-foobar\" }",
              results.back());
  }
}

} // namespace tablet
} // namespace yb
