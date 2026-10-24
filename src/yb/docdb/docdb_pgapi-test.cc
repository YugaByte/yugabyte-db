// Copyright (c) YugabyteDB, Inc.
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

#include <atomic>
#include <string>
#include <vector>

#include "yb/util/logging.h"
#include <gtest/gtest.h>

#include "ybgate/ybgate_api.h"
#include "ybgate/ybgate_status.h"
#include "ybgate/ybgate_api-test.h"

#include "yb/common/common.pb.h"
#include "yb/common/value.pb.h"
#include "yb/docdb/docdb_pgapi.h"
#include "yb/util/env.h"
#include "yb/util/path_util.h"
#include "yb/util/result.h"
#include "yb/util/test_thread_holder.h"
#include "yb/util/test_util.h"
#include "yb/util/yb_pg_errcodes.h"

class YbGateTest : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    google::InitGoogleLogging("");
  }

  void TearDown() override {
    ASSERT_EQ(YBCPgGetThreadLocalJumpBuffer(), nullptr);
  }
};

TEST_F(YbGateTest, ElogLog) {
  auto ybg_status = YbgTest(YBGATE_TEST_ELOG_LOG);
  // elog(LOG) makes status OK
  EXPECT_EQ(YbgStatusIsError(ybg_status), false);
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, ElogNotice) {
  auto ybg_status = YbgTest(YBGATE_TEST_ELOG_NOTICE);
  // elog(NOTICE) makes status OK
  EXPECT_EQ(YbgStatusIsError(ybg_status), false);
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, ElogWarning) {
  auto ybg_status = YbgTest(YBGATE_TEST_ELOG_WARN);
  // elog(WARNING) makes status OK
  EXPECT_EQ(YbgStatusIsError(ybg_status), false);
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, ElogError) {
  auto ybg_status = YbgTest(YBGATE_TEST_ELOG_ERROR);
  // elog(ERROR) makes error status
  EXPECT_EQ(YbgStatusIsError(ybg_status), true);
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, EreportLog) {
  auto ybg_status = YbgTest(YBGATE_TEST_EREPORT_LOG);
  // ereport(LOG) makes status OK
  EXPECT_EQ(YbgStatusIsError(ybg_status), false);
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, EreportWarning) {
  auto ybg_status = YbgTest(YBGATE_TEST_EREPORT_WARN);
  // ereport(WARNING) makes status OK
  EXPECT_EQ(YbgStatusIsError(ybg_status), false);
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, EreportError) {
  auto ybg_status = YbgTest(YBGATE_TEST_EREPORT_ERROR);
  // ereport(ERROR) makes error status
  EXPECT_EQ(YbgStatusIsError(ybg_status), true);
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, EreportFatal) {
  auto ybg_status = YbgTest(YBGATE_TEST_EREPORT_FATAL);
  // ereport(FATAL) makes error status
  EXPECT_EQ(YbgStatusIsError(ybg_status), true);
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, EreportPanic) {
  auto ybg_status = YbgTest(YBGATE_TEST_EREPORT_PANIC);
  // ereport(PANIC) makes error status
  EXPECT_EQ(YbgStatusIsError(ybg_status), true);
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, EreportErrorLocation) {
  auto ybg_status = YbgTest(YBGATE_TEST_EREPORT_ERROR_LOCATION);
  ASSERT_EQ(YbgStatusIsError(ybg_status), true);
  // check status location fields
  EXPECT_STREQ(YbgStatusGetFilename(ybg_status), "ybgate_api-test.c");
  // *** Line number may change if ybgate_api-test.c is modified ***
  EXPECT_EQ(YbgStatusGetLineNumber(ybg_status), 124);
  EXPECT_STREQ(YbgStatusGetFuncname(ybg_status), "yb_test");
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, EreportErrorCode) {
  auto ybg_status = YbgTest(YBGATE_TEST_EREPORT_ERROR_CODE);
  ASSERT_EQ(YbgStatusIsError(ybg_status), true);
  // Check the error code
  EXPECT_EQ(YbgStatusGetSqlError(ybg_status),
            std::to_underlying(yb::YBPgErrorCode::YB_PG_DIVISION_BY_ZERO));
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, EreportFormat) {
  auto ybg_status = YbgTest(YBGATE_TEST_EREPORT_FORMAT);
  ASSERT_EQ(YbgStatusIsError(ybg_status), true);
  int32_t numargs = 0;
  const char **args = YbgStatusGetMessageArgs(ybg_status, &numargs);
  // Assert number of arguments is accurate
  ASSERT_EQ(numargs, 9);
  // then check each individual argument
  EXPECT_STREQ(args[0], "42"); // 2.
  EXPECT_STREQ(args[1], "12.300000"); // 3.
  EXPECT_STREQ(args[2], "foo"); // 4.
  EXPECT_STREQ(args[3], "100"); // 5.
  EXPECT_STREQ(args[4], "  34"); // 6.
  EXPECT_STREQ(args[5], "123456789012"); // 7.
  EXPECT_STREQ(args[6], " 87.65"); // 8.
  EXPECT_STREQ(args[7], "  1024.900"); // 9.
  EXPECT_STREQ(args[8], "+3.579864E+03"); // 10.
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, EreportFormatTooLongArg) {
  auto ybg_status = YbgTest(YBGATE_TEST_EREPORT_FORMAT_TOO_LONG_ARG);
  ASSERT_EQ(YbgStatusIsError(ybg_status), true);
  int32_t numargs = 0;
  const char **args = YbgStatusGetMessageArgs(ybg_status, &numargs);
  // Assert number of arguments is accurate
  ASSERT_EQ(numargs, 10);
  // then check each individual argument
  EXPECT_STREQ(args[0], "[argument is too long]"); // 1.
  EXPECT_STREQ(args[1], "42");                     // 2.
  EXPECT_STREQ(args[2], "9223372036854775807");    // 3.
  EXPECT_STREQ(args[3], "-9223372036854775807");   // 4.
  EXPECT_STREQ(args[4], "18446744073709551615");   // 5.
  EXPECT_STREQ(args[5], "9223372036854775807");    // 6.
  EXPECT_STREQ(args[6], "-9223372036854775807");   // 7.
  EXPECT_STREQ(args[7], "18446744073709551615");   // 8.
  EXPECT_STREQ(args[8], "2.3e-308");               // 9.
  EXPECT_STREQ(args[9], "1.7e+308");               // 10.
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, ElogFormatErrno) {
  auto ybg_status = YbgTest(YBGATE_TEST_ELOG_FORMAT_ERRNO);
  ASSERT_EQ(YbgStatusIsError(ybg_status), true);
  int32_t numargs = 0;
  const char **args = YbgStatusGetMessageArgs(ybg_status, &numargs);
  ASSERT_EQ(numargs, 2);
  // Check %m was rendered properly
  EXPECT_STREQ(args[1], "No such file or directory");
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, EreportAndLog) {
  auto ybg_status = YbgTest(YBGATE_TEST_EREPORT_AND_LOG);
  // Log while forming ereport does not prevent making error status
  ASSERT_EQ(YbgStatusIsError(ybg_status), true);
  // Check the message and the argument are from ereport
  EXPECT_STREQ(YbgStatusGetMessage(ybg_status), "error message: %s");
  EXPECT_STREQ(YbgStatusGetMessageArgs(ybg_status, nullptr)[0], "OK");
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, EreportAndError) {
  auto ybg_status = YbgTest(YBGATE_TEST_EREPORT_AND_ERROR);
  // New ereport while forming ereport makes the status
  ASSERT_EQ(YbgStatusIsError(ybg_status), true);
  // Check the message is from second ereport
  EXPECT_STREQ(YbgStatusGetMessage(ybg_status),
               "another ereport while composing an error");
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, EreportAndTryCatch) {
  auto ybg_status = YbgTest(YBGATE_TEST_EREPORT_AND_TRY_CATCH);
  // Error while forming ereport is caught, so error status is successfully made
  ASSERT_EQ(YbgStatusIsError(ybg_status), true);
  // Check the error message and the argument are from ereport
  EXPECT_STREQ(YbgStatusGetMessage(ybg_status), "error message: %s");
  EXPECT_STREQ(YbgStatusGetMessageArgs(ybg_status, nullptr)[0], "OK");
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, TryCatch) {
  auto ybg_status = YbgTest(YBGATE_TEST_TRY_CATCH);
  // Caught error makes status OK
  EXPECT_EQ(YbgStatusIsError(ybg_status), false);
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, TryCatchRethrow) {
  auto ybg_status = YbgTest(YBGATE_TEST_TRY_CATCH_RETHROW);
  // Rethrown error makes error status
  EXPECT_EQ(YbgStatusIsError(ybg_status), true);
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, TryCatchLog) {
  auto ybg_status = YbgTest(YBGATE_TEST_TRY_CATCH_LOG);
  // Caught error makes status OK, log does not change it
  EXPECT_EQ(YbgStatusIsError(ybg_status), false);
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, TryCatchError) {
  auto ybg_status = YbgTest(YBGATE_TEST_TRY_CATCH_ERROR);
  // Error caught but other one makes error status
  ASSERT_EQ(YbgStatusIsError(ybg_status), true);
  // Check the status carries message from the second error
  EXPECT_STREQ(YbgStatusGetMessage(ybg_status),
               "another ereport while handling an error");
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, TryCatchDoubleError) {
  auto ybg_status = YbgTest(YBGATE_TEST_TRY_CATCH_DOUBLE_ERROR);
  // Error caught but other one makes error status
  ASSERT_EQ(YbgStatusIsError(ybg_status), false);
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, NestedTryTryCatch) {
  auto ybg_status = YbgTest(YBGATE_TEST_NESTED_TRY_TRY_CATCH);
  // Nested error is caught, next ereport makes error status
  ASSERT_EQ(YbgStatusIsError(ybg_status), true);
  // Check the message is from the next error
  EXPECT_STREQ(YbgStatusGetMessage(ybg_status), "direct error");
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, NestedTryTryCatchRethrow) {
  auto ybg_status = YbgTest(YBGATE_TEST_NESTED_TRY_TRY_CATCH_RETHROW);
  // Nested error is caught, but rethrown, so it makes error status
  ASSERT_EQ(YbgStatusIsError(ybg_status), true);
  // Check the message is from the nested error
  EXPECT_STREQ(YbgStatusGetMessage(ybg_status), "nested PG_TRY with ereport");
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, NestedCatchTryCatch) {
  auto ybg_status = YbgTest(YBGATE_TEST_NESTED_CATCH_TRY_CATCH);
  // Nested error is caught
  EXPECT_EQ(YbgStatusIsError(ybg_status), false);
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, NestedCatchTryCatchRethrow) {
  auto ybg_status = YbgTest(YBGATE_TEST_NESTED_CATCH_TRY_CATCH_RETHROW);
  // Nested error is caught and rethrown
  ASSERT_EQ(YbgStatusIsError(ybg_status), true);
  // Check the message is from the nested error
  EXPECT_STREQ(YbgStatusGetMessage(ybg_status), "nested PG_TRY with ereport");
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, EdataLog) {
  auto ybg_status = YbgTest(YBGATE_TEST_EDATA_LOG);
  // Nested error is caught
  EXPECT_EQ(YbgStatusIsError(ybg_status), false);
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, EdataThrow) {
  auto ybg_status = YbgTest(YBGATE_TEST_EDATA_THROW);
  // Nested error is caught
  ASSERT_EQ(YbgStatusIsError(ybg_status), true);
  EXPECT_STREQ(YbgStatusGetMessage(ybg_status), "Modified error");
  YbgStatusDestroy(ybg_status);
}

TEST_F(YbGateTest, NoRepElogLog) {
  // If error is not raised, YbGate works without setting up error handling
  YbgTestNoReporting(YBGATE_TEST_ELOG_LOG);
}

TEST_F(YbGateTest, NoRepElogError) {
  // If error is raised, YbGate crashes without setting up error handling
  EXPECT_DEATH(YbgTestNoReporting(YBGATE_TEST_ELOG_ERROR),
               "PG error reporting has not been set up");
}

TEST_F(YbGateTest, NoRepTryCatch) {
  // If error is raised and caught, YbGate works without setting up error handling
  YbgTestNoReporting(YBGATE_TEST_TRY_CATCH);
}

TEST_F(YbGateTest, NoRepTryCatchRethrow) {
  // If error is caught and rethrown, YbGate crashes without setting up error handling
  EXPECT_DEATH(YbgTestNoReporting(YBGATE_TEST_TRY_CATCH_RETHROW),
               "PG error reporting has not been set up");
}

namespace {

// Regression test for a data race on the process-wide static `struct pg_tm` buffer in
// pg_localtime()/pg_gmtime() (src/postgres/src/timezone/localtime.c). The tserver decodes
// timestamptz values through ybgate from multiple RPC threads, so concurrent decodes used to
// tear each other's fields in that shared buffer. Fixed by making the buffer thread-local.

constexpr int kTimestampTzOid = 1184;
constexpr int kTimestampOid = 1114;
constexpr int kNumDecodeThreads = 8;
constexpr int64_t kDecodeIterations = 200000;
constexpr int64_t kMicrosPerSecond = 1000000;
constexpr int64_t kBaseMicros = 836000000LL * kMicrosPerSecond;

yb::Result<std::string> DecodeDatumString(int pg_type_oid, int64_t micros) {
  yb::QLValuePB ql_value;
  ql_value.set_int64_value(micros);
  yb::DatumMessagePB datum_message;
  RETURN_NOT_OK(
      yb::docdb::SetValueFromQLBinary(
          ql_value, pg_type_oid, {} /* enum_oid_label_map */, {} /* composite_atts_map */,
          datum_message));
  return datum_message.datum_string();
}

void InitDocPg() {
  std::string executable_path;
  ASSERT_OK(yb::Env::Default()->GetExecutablePath(&executable_path));
  const std::string postgres_path = yb::JoinPathSegments(
      yb::DirName(yb::DirName(executable_path)), "postgres", "bin", "postgres");
  ASSERT_OK(yb::docdb::DocPgInit(postgres_path));
}

// Decodes distinct constant values on many threads and compares each result against the
// value decoded single-threaded up front. Before the fix, timestamptz decoding races and
// mismatches appear; the plain timestamp path (no timezone) never touches the shared buffer
// and stays clean.
void CheckConcurrentDecodeIsStable(int pg_type_oid) {
  std::vector<int64_t> values;
  std::vector<std::string> expected;
  for (int i = 0; i < kNumDecodeThreads; ++i) {
    const int64_t micros = kBaseMicros + i * 7 * kMicrosPerSecond + (100 + i * 37) * 1000;
    values.push_back(micros);
    expected.push_back(ASSERT_RESULT(DecodeDatumString(pg_type_oid, micros)));
  }

  std::atomic<int64_t> mismatches{0};
  yb::TestThreadHolder threads;
  for (int i = 0; i < kNumDecodeThreads; ++i) {
    threads.AddThreadFunctor([&, i] {
      for (int64_t iteration = 0; iteration < kDecodeIterations; ++iteration) {
        auto decoded = DecodeDatumString(pg_type_oid, values[i]);
        if (!decoded.ok() || *decoded != expected[i]) {
          mismatches.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  threads.JoinAll();

  ASSERT_EQ(mismatches.load(), 0)
      << "concurrent datum decoding produced corrupted values (pg_type_oid=" << pg_type_oid << ")";
}

}  // namespace

TEST_F(YbGateTest, TimestamptzDecodeIsThreadSafe) {
  InitDocPg();
  ASSERT_FALSE(HasFatalFailure());
  CheckConcurrentDecodeIsStable(kTimestampTzOid);
}

TEST_F(YbGateTest, TimestampDecodeIsThreadSafe) {
  InitDocPg();
  ASSERT_FALSE(HasFatalFailure());
  CheckConcurrentDecodeIsStable(kTimestampOid);
}
