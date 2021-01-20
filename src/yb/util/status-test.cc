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
// Some portions Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <errno.h>

#include <vector>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "yb/util/errno.h"
#include "yb/util/random_util.h"
#include "yb/util/status.h"
#include "yb/util/test_util.h"

using std::string;
using namespace std::literals;

namespace yb {

template <uint8_t category>
struct TestErrorTag : IntegralErrorTag<int64_t> {
  static constexpr uint8_t kCategory = category;

  static std::string ToMessage(Value value) {
    return std::to_string(value);
  }
};

template <uint8_t category>
using TestError = StatusErrorCodeImpl<TestErrorTag<category>>;

class TestErrorDescriptor {
 public:
  virtual void ApplyTo(Status* status, int value) = 0;
  virtual void Check(const Status& status, int value) = 0;
  virtual ~TestErrorDescriptor() = default;
};

template <uint8_t category>
class TestErrorDescriptorImpl : public TestErrorDescriptor {
 public:
  void ApplyTo(Status* status, int value) override {
    *status = status->CloneAndAddErrorCode(TestError<category>(value));
  }

  void Check(const Status& status, int value) override {
    TestError<category> ec(status);
    ASSERT_EQ(ec.value(), value);
  }
};

constexpr uint8_t kMinTestError = 202;
constexpr uint8_t kMaxTestError = 210;

std::array<std::unique_ptr<TestErrorDescriptor>, kMaxTestError + 1> kTestErrorDescriptors;

template <uint8_t category>
StatusCategoryRegisterer& RegisterTestError() {
  static const std::string kTestErrorCategoryName = "test error " + std::to_string(category);

  static StatusCategoryRegisterer test_error_category_registerer(
      StatusCategoryDescription::Make<TestErrorTag<category>>(&kTestErrorCategoryName));
  kTestErrorDescriptors[category] = std::make_unique<TestErrorDescriptorImpl<category>>();
  return test_error_category_registerer;
}

template <uint8_t category>
struct RegisterTestErrors {
  RegisterTestErrors() {
    RegisterTestErrors<category - 1> temp;
    RegisterTestError<category>();
  }
};

template <>
struct RegisterTestErrors<kMinTestError> {
  RegisterTestErrors() {
    RegisterTestError<kMinTestError>();
  }
};

RegisterTestErrors<kMaxTestError> register_test_errors;

class ErrorDelayTraits {
 public:
  typedef MonoDelta ValueType;
  typedef int64_t RepresentationType;

  static MonoDelta FromRepresentation(RepresentationType source) {
    return MonoDelta::FromNanoseconds(source);
  }

  static RepresentationType ToRepresentation(MonoDelta value) {
    return value.ToNanoseconds();
  }

  static std::string ToString(MonoDelta value) {
    return value.ToString();
  }
};

class ErrorDelayTag : public IntegralBackedErrorTag<ErrorDelayTraits> {
 public:
  static constexpr uint8_t kCategory = kMaxTestError + 1;

  static std::string ToMessage(MonoDelta value) {
    return value.ToString();
  }
};

typedef StatusErrorCodeImpl<ErrorDelayTag> ErrorDelay;

const std::string kErrorDelayCategoryName = "error delay";

static StatusCategoryRegisterer error_delay_category_registerer(
    StatusCategoryDescription::Make<ErrorDelayTag>(&kErrorDelayCategoryName));

class StringVectorErrorTag : public StringVectorBackedErrorTag {
 public:
  static constexpr uint8_t kCategory = kMaxTestError + 2;

  static std::string ToMessage(Value value) {
    return AsString(value);
  }
};

typedef StatusErrorCodeImpl<StringVectorErrorTag> StringVectorError;

const std::string kStringVectorErrorCategoryName = "string vector error";

static StatusCategoryRegisterer string_vector_error_category_registerer(
    StatusCategoryDescription::Make<StringVectorErrorTag>(&kStringVectorErrorCategoryName));

TEST(StatusTest, TestPosixCode) {
  Status ok = Status::OK();
  ASSERT_EQ(0, Errno(ok));
  Status file_error = STATUS(IOError, "file error", Slice(), Errno(ENOTDIR));
  ASSERT_EQ(ENOTDIR, Errno(file_error));
}

TEST(StatusTest, TestToString) {
  Status file_error = STATUS(IOError, "file error", Slice(), Errno(ENOTDIR));
  ASSERT_EQ("IO error: file error (system error 20)", file_error.ToString(false));
}

TEST(StatusTest, TestClonePrepend) {
  Status file_error = STATUS(IOError, "file error", "msg2", Errno(ENOTDIR));
  Status appended = file_error.CloneAndPrepend("Heading");
  ASSERT_EQ("IO error: Heading: file error: msg2 (system error 20)",
            appended.ToString(false));
}

TEST(StatusTest, TestCloneAppend) {
  Status remote_error = STATUS(RemoteError, "Application error");
  Status appended = remote_error.CloneAndAppend(STATUS(NotFound, "Unknown tablet").ToString(false));
  ASSERT_EQ(string("Remote error: Application error: Not found: Unknown tablet"),
            appended.ToString(false));
}

TEST(StatusTest, CloneAndAddErrorCode) {
  Status remote_error = STATUS(RemoteError, "ERROR", Slice(), Errno(ENOTDIR));
  Status appended = remote_error.CloneAndAddErrorCode(TestError<kMinTestError>(42));
  ASSERT_EQ(Format("Remote error: ERROR (system error 20) (test error $0 42)", kMinTestError),
            appended.ToString(false));
}

TEST(StatusTest, RandomErrorCodes) {
  for (int i = 0; i != 1000; ++i) {
    auto status = STATUS(RemoteError, "ERROR");
    int num_extra_errors = RandomUniformInt(1, 10);
    std::map<uint8_t, int> extra_errors;
    LOG(INFO) << "===================================================";
    for (int j = 0; j != num_extra_errors; ++j) {
      auto error_code = RandomUniformInt(kMinTestError, kMaxTestError);
      auto value = RandomUniformInt(1, 1000);
      LOG(INFO) << "Add: " << static_cast<int>(error_code) << ", " << value;
      extra_errors[error_code] = value;
      kTestErrorDescriptors[error_code]->ApplyTo(&status, value);

      std::string str = "Remote error: ERROR";
      for (auto ec = kMinTestError; ec <= kMaxTestError; ++ec) {
        SCOPED_TRACE(Format("Error code: $0", static_cast<int>(ec)));
        auto it = extra_errors.find(ec);
        if (it != extra_errors.end()) {
          ASSERT_NO_FATALS(kTestErrorDescriptors[ec]->Check(status, it->second));
          str += Format(" (test error $0 $1)", ec, it->second);
        } else {
          ASSERT_NO_FATALS(kTestErrorDescriptors[ec]->Check(status, 0))
              << "Extra errors: " << yb::ToString(extra_errors);
        }
      }
      ASSERT_EQ(str, status.ToString(false));
      LOG(INFO) << "To string: " << str;
    }
  }
}



TEST(StatusTest, TestMemoryUsage) {
  ASSERT_EQ(0, static_cast<Status>(Status::OK()).memory_footprint_excluding_this());
  auto status = STATUS(IOError, "file error", "some other thing", Errno(ENOTDIR));
  ASSERT_GT(status.memory_footprint_excluding_this(), 0);
}

TEST(StatusTest, TestMoveConstructor) {
  // OK->OK move should do nothing.
  {
    Status src = Status::OK();
    Status dst = std::move(src);
    ASSERT_OK(src);
    ASSERT_OK(dst);
  }

  // Moving a not-OK status into a new one should make the moved status
  // "OK".
  {
    Status src = STATUS(NotFound, "foo");
    Status dst = std::move(src);
    ASSERT_OK(src);
    ASSERT_EQ("Not found: foo", dst.ToString(false));
  }
}

TEST(StatusTest, TestMoveAssignment) {
  // OK->Bad move should clear the source status and also make the
  // destination status OK.
  {
    Status src = Status::OK();
    Status dst = STATUS(NotFound, "orig dst");
    dst = std::move(src);
    ASSERT_OK(src);
    ASSERT_OK(dst);
  }

  // Bad->Bad move.
  {
    Status src = STATUS(NotFound, "orig src");
    Status dst = STATUS(NotFound, "orig dst");
    dst = std::move(src);
    ASSERT_OK(src);
    ASSERT_EQ("Not found: orig src", dst.ToString(false));
  }

  // Bad->OK move
  {
    Status src = STATUS(NotFound, "orig src");
    Status dst = Status::OK();
    dst = std::move(src);
    ASSERT_OK(src);
    ASSERT_EQ("Not found: orig src", dst.ToString(false));
  }
}

TEST(StatusTest, IntegralBackedError) {
  MonoDelta delay = 100ms;
  auto status = STATUS(TimedOut, "TEST", ErrorDelay(delay));
  LOG(INFO) << status;
  ASSERT_EQ(ErrorDelay(status), delay);
}

TEST(StatusTest, StringVectorError) {
  std::vector<std::string> vector;
  for (int i = 0; i <= 3; ++i) {
    auto status = STATUS(TimedOut, "TEST", StringVectorError(vector));
    LOG(INFO) << status;
    ASSERT_EQ(StringVectorError(status), vector);
    std::string str("TEST_");
    for (int j = 0; j <= i; ++j) {
      str.append(AsString(j));
    }
    vector.push_back(str);
  }
}

}  // namespace yb
