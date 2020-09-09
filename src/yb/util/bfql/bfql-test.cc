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
// This is a simple test to verify the correctness of builtin function library.

#include <memory>
#include <vector>

#include "yb/util/bfql/bfql.h"
#include "yb/util/net/net_util.h"
#include "yb/util/test_util.h"
#include "yb/common/ql_value.h"

namespace yb {
namespace bfql {

using std::shared_ptr;
using std::make_shared;
using std::to_string;
using std::vector;
using std::numeric_limits;
//--------------------------------------------------------------------------------------------------
// BFTestValue is a data value to be used with builtin library for both phases - compilation and
// execution. Note that the plan is to have two different data structures for two different.
// - QL treenode is used during compilation.
// - QLValue is used during execution.
class BFTestValue : public QLValue {
 public:
  typedef std::shared_ptr<BFTestValue> SharedPtr;
  typedef std::shared_ptr<const BFTestValue> SharedPtrConst;

  BFTestValue() : QLValue() {
    ql_type_id_ = DataType::UNKNOWN_DATA;
  }

  virtual DataType ql_type_id() const {
    return ql_type_id_;
  }
  virtual void set_ql_type_id(DataType ql_type_id) {
    ql_type_id_ = ql_type_id;
  }

 private:
  DataType ql_type_id_;
};

//--------------------------------------------------------------------------------------------------
// Construct BFApiTest classes from the builtin template. This test is kept to be generic for all
// applications, so it defines its own type. For specific application, appropriate datatypes should
// be used in its tests.
using BFCompileApiTest = BFCompileApi<BFTestValue, BFTestValue>;
using BFExecApiTest = BFExecImmediateApi<BFTestValue, BFTestValue>;

//--------------------------------------------------------------------------------------------------
// This is the driver for bfql-test.
class BfqlTest : public YBTest {
 public:
  // Constructor and destructor.
  BfqlTest() {
  }

  virtual ~BfqlTest() {
  }

  //------------------------------------------------------------------------------------------------
  // Test start and cleanup functions.
  void SetUp() override {
    YBTest::SetUp();
  }

  void TearDown() override {
    YBTest::TearDown();
  }

  // Convert the param values to datatypes that are specified by "bfdecl".
  Status ConvertParams(const BFDecl *bfdecl,
                       const vector<BFTestValue::SharedPtr>& params,
                       vector<BFTestValue::SharedPtr> *converted_params) {
    const int pcount = params.size();
    converted_params->resize(pcount);
    const std::vector<DataType>& ptypes = bfdecl->param_types();

    bool is_variadic = false;
    vector<BFTestValue::SharedPtr> cast_params(2);
    for (int pindex = 0; pindex < pcount; pindex++) {
      if (is_variadic || ptypes[pindex] == DataType::TYPEARGS) {
        // No conversion is needed for the rest of the arguments.
        is_variadic = true;
        (*converted_params)[pindex] = params[pindex];

      } else if (params[pindex]->ql_type_id() == ptypes[pindex]) {
        (*converted_params)[pindex] = params[pindex];

      } else {
        // Casting is needed.
        BFTestValue::SharedPtr converted_param = make_shared<BFTestValue>();
        converted_param->set_ql_type_id(ptypes[pindex]);

        // Converting params.
        cast_params[0] = params[pindex];
        cast_params[1] = converted_param;
        BFExecApiTest::ExecQLFunc(bfql::kCastFuncName, cast_params, converted_param);

        // Save converted value.
        (*converted_params)[pindex] = converted_param;
      }
    }

    return Status::OK();
  }

  // Test token() and partition_hash().
  template <typename ResultVal, typename BuiltinFunction>
  void testPartitionHash(const std::string& name, DataType return_type, ResultVal result_val,
                         BuiltinFunction builtin_function) {
    BFTestValue::SharedPtr result = make_shared<BFTestValue>();
    vector<BFTestValue::SharedPtr> test_params(8);
    for (int pindex = 0; pindex < 8; pindex++) {
      test_params[pindex] = make_shared<BFTestValue>();
    }
    test_params[0]->set_int8_value(100);
    test_params[1]->set_int16_value(200);
    test_params[2]->set_int32_value(300);
    test_params[3]->set_int64_value(400);
    test_params[4]->set_timestamp_value(500);
    test_params[5]->set_string_value("600");
    test_params[7]->set_binary_value("700");

    Uuid uuid;
    ASSERT_OK(uuid.FromString("80000000-0000-0000-0000-000000000000"));
    test_params[6]->set_uuid_value(uuid);

    // Convert test_params to params.
    vector<BFTestValue::SharedPtr> params(test_params.begin(), test_params.end());

    // Use wrong return type and expect error.
    // NOTES:
    // - BFExecApiTest::ExecQLFunc("builin_name") will combine the two steps of finding and
    //   executing opcode into one function call. This is only convenient for testing. In actual
    //   code, except execute-immediate feature, this process is divided into two steps.
    result->set_ql_type_id(DataType::STRING);
    ASSERT_NOK(BFExecApiTest::ExecQLFunc(name, params, result));

    // Use correct return type.
    result->set_ql_type_id(return_type);
    ASSERT_OK(BFExecApiTest::ExecQLFunc(name, params, result));

    // Call the C++ function directly and verify result.
    BFTestValue::SharedPtr expected_result = make_shared<BFTestValue>();
    ASSERT_OK(builtin_function(params, expected_result));
    ASSERT_EQ(result_val(result), result_val(expected_result));

    // Convert shared_ptr to raw pointer to test the API for raw pointers.
    BFTestValue *raw_result = result.get();
    vector<BFTestValue*> raw_params(8);
    for (int pindex = 0; pindex < 8; pindex++) {
      raw_params[pindex] = params[pindex].get();
    }

    // Use wrong return type and expect error.
    raw_result->set_ql_type_id(DataType::STRING);
    ASSERT_NOK(BFExecApiTest::ExecQLFunc(name, raw_params, raw_result));

    // Use correct return type.
    raw_result->set_ql_type_id(return_type);
    ASSERT_OK(BFExecApiTest::ExecQLFunc(name, raw_params, raw_result));

    // Call the C++ function directly and verify result.
    ASSERT_OK(builtin_function(params, expected_result));
    ASSERT_EQ(result_val(result), result_val(expected_result));
  }
};

//--------------------------------------------------------------------------------------------------
// The following test cases generally go through the following steps.
//
// - Use the BFTestValue functions to set the values and datatypes for parameter.
// - Similarly, use the BFTestValue functions to set the datatypes for return result.
//   NOTE: When the return type is not set, builtin-library will set it. On the other hand, if the
//   return type is set, the builtin-library will check if the datatype is compatible with the
//   definition of the builtin function.
// - Call FindQLOpcode() to find the opcode. This step should be done during compilation.
// - Call ExecQLFunc() to run the builtin function. This step will be done during execution.

// Test calls to functions with arguments whose datatypes are an exact matched to the signature.
TEST_F(BfqlTest, TestExactMatchSignature) {
  BFOpcode opcode;
  const BFDecl *bfdecl;

  BFTestValue::SharedPtr result = make_shared<BFTestValue>();
  BFTestValue::SharedPtr param0 = make_shared<BFTestValue>();
  BFTestValue::SharedPtr param1 = make_shared<BFTestValue>();

  // Use raw pointer to test the API for raw pointers.
  vector<BFTestValue::SharedPtr> params = { param0, param1 };

  //------------------------------------------------------------------------------------------------
  // Test cases of exact match calls for integer functions.
  param0->set_ql_type_id(DataType::INT64);
  param1->set_ql_type_id(DataType::INT64);

  // Test Case 1: Not specify the return type by setting it to NULL.
  //    UNKNOWN = INT64 + INT64.
  // Set result type to be unknown and let builtin library resolve its type.
  result->set_ql_type_id(DataType::UNKNOWN_DATA);

  // Initialize parameter values.
  int int_val1 = 2133;
  int int_val2 = 1234;
  param0->set_int64_value(int_val1);
  param1->set_int64_value(int_val2);

  // Find and execute the opcode.
  ASSERT_OK(BFCompileApiTest::FindQLOpcode("+", params, &opcode, &bfdecl, result));
  ASSERT_OK(BFExecApiTest::ExecQLOpcode(opcode, params, result));

  // Write the result to an integer and check the result.
  int expected_int_result = int_val1 + int_val2;
  int return_int_result = result->int64_value();
  ASSERT_EQ(return_int_result, expected_int_result);

  // Test Case 2: The return type is exact match
  //    INT64 = INT64 + INT64.
  // Similar to test #1, but set result type to INT32 let builtin library does the type-checking.
  result->set_ql_type_id(DataType::INT64);
  int_val1 = 4133;
  int_val2 = 7234;
  param0->set_int64_value(int_val1);
  param1->set_int64_value(int_val2);
  ASSERT_OK(BFCompileApiTest::FindQLOpcode("+", params, &opcode, &bfdecl, result));
  ASSERT_EQ(result->ql_type_id(), DataType::INT64);
  ASSERT_OK(BFExecApiTest::ExecQLOpcode(opcode, params, result));

  expected_int_result = int_val1 + int_val2;
  return_int_result = result->int64_value();
  ASSERT_EQ(return_int_result, expected_int_result);

  // Test Case 3: The return type is compatible to force a conversion for return value.
  //    INT16 = INT64 + INT64.
  // Similar to test #1, but set result type to INT16.
  BFTestValue::SharedPtr temp_result = make_shared<BFTestValue>();
  temp_result->set_ql_type_id(DataType::UNKNOWN_DATA);

  int_val1 = 3133;
  int_val2 = 9234;
  param0->set_int64_value(int_val1);
  param1->set_int64_value(int_val2);
  ASSERT_OK(BFCompileApiTest::FindQLOpcode("+", params, &opcode, &bfdecl, temp_result));
  ASSERT_EQ(temp_result->ql_type_id(), DataType::INT64);
  ASSERT_OK(BFExecApiTest::ExecQLOpcode(opcode, params, temp_result));

  // Convert int64 value (temp_result) to int16 value (result).
  result->set_ql_type_id(DataType::INT16);
  vector<BFTestValue::SharedPtr> temp_params = { temp_result, result };
  BFExecApiTest::ExecQLFunc(bfql::kCastFuncName, temp_params, result);

  // Check result.
  expected_int_result = int_val1 + int_val2;
  return_int_result = result->int16_value();
  ASSERT_EQ(return_int_result, expected_int_result);

  //------------------------------------------------------------------------------------------------
  // Test exact match calls for double functions.
  // The steps in this test is the same as for integer.
  // Parameter type is now set to double.
  param0->set_ql_type_id(DataType::DOUBLE);
  param1->set_ql_type_id(DataType::DOUBLE);

  // - Case 1: Not specify the return type by setting it to NULL.
  //     UNKNOWN = DOUBLE + DOUBLE.
  result->set_ql_type_id(DataType::UNKNOWN_DATA);

  double d_val1 = 777.7;
  double d_val2 = 1111.7;
  param0->set_double_value(d_val1);
  param1->set_double_value(d_val2);
  ASSERT_OK(BFExecApiTest::ExecQLFunc("+", params, result));
  ASSERT_EQ(result->ql_type_id(), DataType::DOUBLE);

  // Write the return value to an int so that we can run EQ check.
  expected_int_result = d_val1 + d_val2;
  return_int_result = result->double_value();
  ASSERT_EQ(return_int_result, expected_int_result);

  // - Case 2: Have exact match return type.
  //     DOUBLE = DOUBLE + DOUBLE.
  result->set_ql_type_id(DataType::DOUBLE);

  d_val1 = 999.9;
  d_val2 = 3333.3;
  param0->set_double_value(d_val1);
  param1->set_double_value(d_val2);
  ASSERT_OK(BFExecApiTest::ExecQLFunc("+", params, result));

  expected_int_result = d_val1 + d_val2;
  return_int_result = result->double_value();
  ASSERT_EQ(return_int_result, expected_int_result);

  // - Case 3: Have compatible return type.
  //     FLOAT = DOUBLE + DOUBLE.
  d_val1 = 888.9;
  d_val2 = 8888.3;
  param0->set_double_value(d_val1);
  param1->set_double_value(d_val2);

  // Execute (double + double) and convert double(temp_result) to float(result).
  result->set_ql_type_id(DataType::FLOAT);
  temp_result->set_ql_type_id(DataType::DOUBLE);
  ASSERT_OK(BFExecApiTest::ExecQLFunc("+", params, temp_result));
  ASSERT_OK(BFExecApiTest::ExecQLFunc("cast", temp_params, result));

  expected_int_result = d_val1 + d_val2;
  return_int_result = result->float_value();
  ASSERT_EQ(return_int_result, expected_int_result);

  //------------------------------------------------------------------------------------------------
  // Test exact match calls for string functions.
  // Test case: STRING = STRING + STRING
  result->set_ql_type_id(DataType::STRING);

  param0->set_ql_type_id(DataType::STRING);
  param1->set_ql_type_id(DataType::STRING);
  param0->set_string_value("First part of String. ");
  param1->set_string_value("Second part of String.");

  ASSERT_OK(BFExecApiTest::ExecQLFunc("+", params, result));
  ASSERT_EQ(result->string_value(), "First part of String. Second part of String.");
}

// Test calls to functions with arguments whose datatypes are convertible but not an exact match to
// the function signature.
TEST_F(BfqlTest, TestCompatibleSignature) {
  BFOpcode opcode;
  const BFDecl *bfdecl;

  BFTestValue::SharedPtr result = make_shared<BFTestValue>();
  BFTestValue::SharedPtr param0 = make_shared<BFTestValue>();
  BFTestValue::SharedPtr param1 = make_shared<BFTestValue>();

  // Use shared pointer to test the API for shared_ptr.
  vector<BFTestValue::SharedPtr> params = { param0, param1 };
  vector<BFTestValue::SharedPtr> converted_params;

  //------------------------------------------------------------------------------------------------
  // Test case: Passing (STRING, INT16) to (STRING, DOUBLE)

  // Set result type to be unknown and let builtin library resolve its type.
  result->set_ql_type_id(DataType::UNKNOWN_DATA);

  // Initialize parameter datatypes and values.
  param0->set_ql_type_id(DataType::STRING);
  param1->set_ql_type_id(DataType::INT16);
  param0->set_string_value("The value is ");
  param1->set_int16_value(100);

  // Find the opcode.
  ASSERT_OK(BFCompileApiTest::FindQLOpcode("+", params, &opcode, &bfdecl, result));
  ConvertParams(bfdecl, params, &converted_params);

  // Execute the opcode.
  ASSERT_OK(BFExecApiTest::ExecQLOpcode(opcode, converted_params, result));

  // Write the result to a string and check the result.
  string expected_result = string("The value is ") + to_string(100.);
  string return_result = result->string_value();
  ASSERT_EQ(return_result, expected_result);

  //------------------------------------------------------------------------------------------------
  // Test case: Passing (INT64, STRING) to (DOUBLE, STRING)

  // Set result type to be unknown and let builtin library resolve its type.
  result->set_ql_type_id(DataType::UNKNOWN_DATA);

  // Initialize parameter datatypes and values.
  param0->set_ql_type_id(DataType::INT64);
  param1->set_ql_type_id(DataType::STRING);
  param0->set_int64_value(100);
  param1->set_string_value(" is the value");

  // Find the opcode.
  ASSERT_OK(BFCompileApiTest::FindQLOpcode("+", params, &opcode, &bfdecl, result));
  ConvertParams(bfdecl, params, &converted_params);

  // Execute the opcode.
  ASSERT_OK(BFExecApiTest::ExecQLOpcode(opcode, converted_params, result));

  // Write the result to a string and check the result.
  expected_result = to_string(100.) + string(" is the value");
  return_result = result->string_value();
  ASSERT_EQ(return_result, expected_result);
}

// Test variadic function calls (TOKEN).
TEST_F(BfqlTest, TestVariadicBuiltinToken) {
  testPartitionHash("token", DataType::INT64,
                    [](BFTestValue::SharedPtr val) -> int64_t { return val->int64_value(); },
                    [](const vector<BFTestValue::SharedPtr>& params,
                       BFTestValue::SharedPtr expected_result) -> Status {
                      return Token<BFTestValue::SharedPtr, BFTestValue::SharedPtr>(params,
                                                                                   expected_result);
                    });
}

// Test variadic function calls (PARTITION_HASH).
TEST_F(BfqlTest, TestVariadicBuiltinPartitionHash) {
  testPartitionHash("partition_hash", DataType::INT32,
                    [](BFTestValue::SharedPtr val) -> int32_t { return val->int32_value(); },
                    [](const vector<BFTestValue::SharedPtr>& params,
                       BFTestValue::SharedPtr expected_result) -> Status {
                      return PartitionHash<BFTestValue::SharedPtr, BFTestValue::SharedPtr>(
                          params, expected_result);
                    });
}

// Test bad function calls.
TEST_F(BfqlTest, TestErroneousFuncCalls) {
  BFOpcode opcode;
  const BFDecl *bfdecl;

  BFTestValue::SharedPtr result = make_shared<BFTestValue>();
  vector<BFTestValue::SharedPtr> params;

  // Invalid function name.
  ASSERT_NOK(BFCompileApiTest::FindQLOpcode("wrong_name", params, &opcode, &bfdecl, result));

  //------------------------------------------------------------------------------------------------
  // Test for invalid parameter count.
  // Passing 0 argument to '+', which takes exactly 2 arguments.
  ASSERT_NOK(BFCompileApiTest::FindQLOpcode("+", params, &opcode, &bfdecl, result));

  // Passing 1 argument to '+', which takes exactly 2 arguments.
  BFTestValue::SharedPtr param0 = make_shared<BFTestValue>();
  params.push_back(param0);
  param0->set_ql_type_id(DataType::INT32);
  ASSERT_NOK(BFCompileApiTest::FindQLOpcode("+", params, &opcode, &bfdecl, result));

  BFTestValue::SharedPtr param1 = make_shared<BFTestValue>();
  param1->set_ql_type_id(DataType::INT32);
  params.push_back(param1);

  // Passing 3 arguments to '+', which takes exactly 2 arguments.
  BFTestValue::SharedPtr param2 = make_shared<BFTestValue>();
  param2->set_ql_type_id(DataType::INT32);
  params.push_back(param2);
  ASSERT_NOK(BFCompileApiTest::FindQLOpcode("+", params, &opcode, &bfdecl, result));

  //------------------------------------------------------------------------------------------------
  // Test for invalid parameter types.
  params.resize(2);
  param0->set_ql_type_id(DataType::INT32);
  param1->set_ql_type_id(DataType::BOOL);
  ASSERT_NOK(BFCompileApiTest::FindQLOpcode("+", params, &opcode, &bfdecl, result));

  param0->set_ql_type_id(DataType::BOOL);
  param1->set_ql_type_id(DataType::INT32);
  ASSERT_NOK(BFCompileApiTest::FindQLOpcode("+", params, &opcode, &bfdecl, result));

  //------------------------------------------------------------------------------------------------
  // Test for invalid return type.
  // The builtin call will set the return-type after its evaluate the function calls. Howver, if
  // the return_type is set by application, the builtin-call will check if the given return type
  // is compatible.
  param0->set_ql_type_id(DataType::INT32);
  param1->set_ql_type_id(DataType::INT32);

  result->set_ql_type_id(DataType::STRING);
  ASSERT_NOK(BFCompileApiTest::FindQLOpcode("+", params, &opcode, &bfdecl, result));

  result->set_ql_type_id(DataType::BOOL);
  ASSERT_NOK(BFCompileApiTest::FindQLOpcode("+", params, &opcode, &bfdecl, result));

  //------------------------------------------------------------------------------------------------
  // Test for ambiguous signature - Too many builtin match the signature of a function call.
  // The following can be matched with both signature INT(INT, INT) and DOUBLE(DOUBLE, DOUBLE).
  param0->set_ql_type_id(DataType::INT8);
  param1->set_ql_type_id(DataType::INT8);
  ASSERT_NOK(BFCompileApiTest::FindQLOpcode("+", params, &opcode, &bfdecl, result));
}

TEST_F(BfqlTest, TestBuiltinToJson) {
  auto check_tojson = [](const BFTestValue& test_value, const string& expected) -> void {
    auto result_val = [](BFTestValue::SharedPtr val) -> string {
                        string result;
                        common::Jsonb jsonb(val->jsonb_value());
                        CHECK_OK(jsonb.ToJsonString(&result));
                        return result;
                      };

    BFTestValue::SharedPtr result = make_shared<BFTestValue>();
    vector<BFTestValue::SharedPtr> params(1);
    params[0] = make_shared<BFTestValue>();
    *(params[0]->mutable_value()) = test_value.value();

    // Use wrong return type and expect error.
    // NOTES:
    // - BFExecApiTest::ExecQLFunc("builin_name") will combine the two steps of finding and
    //   executing opcode into one function call. This is only convenient for testing. In actual
    //   code, except execute-immediate feature, this process is divided into two steps.
    result->set_ql_type_id(DataType::STRING);
    ASSERT_NOK(BFExecApiTest::ExecQLFunc("tojson", params, result));

    // Use correct return type.
    result->set_ql_type_id(DataType::JSONB);
    ASSERT_OK(BFExecApiTest::ExecQLFunc("tojson", params, result));
    LOG(INFO) << "tojson(" << test_value.ToString() << ")=" << result_val(result);
    ASSERT_EQ(result_val(result), expected);

    // Call the C++ function directly and verify result.
    BFTestValue::SharedPtr expected_result = make_shared<BFTestValue>();
    ASSERT_OK(ToJson(params[0], expected_result));
    ASSERT_EQ(result_val(result), result_val(expected_result));
  };

  // CQL types: NULL, INT8, INT16, INT32, INT64, STRING, BOOL, FLOAT, DOUBLE, BINARY, TIMESTAMP,
  //            DECIMAL, VARINT, INET, LIST, MAP, SET, UUID, TIMEUUID, TUPLE, TYPEARGS,
  //            USER_DEFINED_TYPE, FROZEN, DATE, TIME, JSONB,
  BFTestValue param;

  // Test simple values.

  param.SetNull();
  check_tojson(param, "null");

  param.set_int8_value(-100);
  check_tojson(param, "-100");

  param.set_int16_value(-200);
  check_tojson(param, "-200");

  param.set_int32_value(-300);
  check_tojson(param, "-300");

  param.set_int64_value(-400);
  check_tojson(param, "-400");

  param.set_float_value(124.125f);
  check_tojson(param, "124.125");

  param.set_float_value(numeric_limits<float>::quiet_NaN());
  check_tojson(param, "null");

  param.set_float_value(-numeric_limits<float>::quiet_NaN());
  check_tojson(param, "null");

  param.set_float_value(numeric_limits<float>::infinity());
  check_tojson(param, "null");

  param.set_float_value(-numeric_limits<float>::infinity());
  check_tojson(param, "null");

  param.set_double_value(124.125);
  check_tojson(param, "124.125");

  param.set_bool_value(true);
  check_tojson(param, "true");

  // Note: conversion into internal JSON 'double' can overflow.
  util::Decimal d("-9847.125");
  param.set_decimal_value(d.EncodeToComparable());
  check_tojson(param, "-9847.125");

  util::Decimal d2("-987654321");
  param.set_decimal_value(d2.EncodeToComparable());
  check_tojson(param, "-987654321");

  // Note: conversion into internal JSON 'double' can overflow.
  Result<util::VarInt> varint = util::VarInt::CreateFromString("-1234567890");
  ASSERT_TRUE(varint.ok());
  param.set_varint_value(*varint);
  check_tojson(param, "-1234567890");

  param.set_string_value("value");
  check_tojson(param, "\"value\"");

  Result<uint32_t> date = DateTime::DateFromString("2018-02-14");
  ASSERT_TRUE(date.ok());
  param.set_date_value(*date);
  check_tojson(param, "\"2018-02-14\"");

  Result<int64_t> time = DateTime::TimeFromString("01:02:03.123456789");
  ASSERT_TRUE(time.ok());
  param.set_time_value(*time);
  check_tojson(param, "\"01:02:03.123456789\"");

  string ts_str = "2018-2-14 13:24:56.987+01:00";
  Result<Timestamp> ts = DateTime::TimestampFromString(ts_str);
  ASSERT_OK(ts);
  param.set_timestamp_value(ts->ToInt64());
  // TODO: Check output format.
  check_tojson(param, "\"2018-02-14T12:24:56.987000+0000\"");

  Uuid uuid;
  string uuid_str = "87654321-dead-beaf-0000-deadbeaf0000";
  ASSERT_OK(uuid.FromString(uuid_str));
  param.set_uuid_value(uuid);
  check_tojson(param, "\"" + uuid_str + "\"");

  uuid_t linux_time_uuid;
  uuid_generate_time(linux_time_uuid);
  Uuid time_uuid(linux_time_uuid);
  ASSERT_OK(time_uuid.IsTimeUuid());
  ASSERT_OK(time_uuid.HashMACAddress());
  param.set_timeuuid_value(time_uuid);
  string time_uuid_str = time_uuid.ToString();
  check_tojson(param, "\"" + time_uuid_str + "\"");

  InetAddress addr(ASSERT_RESULT(ParseIpAddress("1.2.3.4")));
  param.set_inetaddress_value(addr);
  check_tojson(param, "\"1.2.3.4\"");

  param.set_binary_value("ABC");
  check_tojson(param, "\"0x414243\"");

  string json_str = "{\"a\":{\"b\":321},\"c\":[{\"d\":123}]}";
  common::Jsonb jsonb;
  ASSERT_OK(jsonb.FromString(json_str));
  param.set_jsonb_value(jsonb.SerializedJsonb());
  check_tojson(param, json_str);

  // Test collections.
  param.add_map_key()->set_string_value("a");
  param.add_map_value()->set_int32_value(21);
  param.add_map_key()->set_string_value("b");
  param.add_map_value()->set_int32_value(22);
  check_tojson(param, "{\"a\":21,\"b\":22}");

  param.add_map_key()->set_string_value("c");
  param.add_map_value()->set_string_value("23");
  check_tojson(param, "{\"a\":21,\"b\":22,\"c\":\"23\"}");

  param.add_map_key()->set_int32_value(88);
  param.add_map_value()->set_string_value("24");
  check_tojson(param, "{\"88\":\"24\",\"a\":21,\"b\":22,\"c\":\"23\"}");

  param.add_map_key()->set_int32_value(99);
  param.add_map_value()->set_int32_value(25);
  check_tojson(param, "{\"88\":\"24\",\"99\":25,\"a\":21,\"b\":22,\"c\":\"23\"}");

  param.add_set_elem()->set_string_value("a");
  param.add_set_elem()->set_string_value("b");
  check_tojson(param, "[\"a\",\"b\"]");

  param.add_list_elem()->set_int32_value(123);
  param.add_list_elem()->set_int32_value(456);
  check_tojson(param, "[123,456]");

  // Test FROZEN<SET> or FROZEN<LIST>.
  param.add_frozen_elem()->set_int32_value(789);
  param.add_frozen_elem()->set_int32_value(123);
  check_tojson(param, "[789,123]");
}

} // namespace bfql
} // namespace yb
