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

#include <gtest/gtest.h>

#include "yb/util/string_case.h"

using std::string;

namespace yb {

TEST(TestStringCase, TestSnakeToCamel) {
  string out;
  SnakeToCamelCase("foo_bar", &out);
  ASSERT_EQ("FooBar", out);


  SnakeToCamelCase("foo-bar", &out);
  ASSERT_EQ("FooBar", out);

  SnakeToCamelCase("foobar", &out);
  ASSERT_EQ("Foobar", out);
}

TEST(TestStringCase, TestAllCapsToCamel) {
  string out;
  AllCapsToCamelCase("FOO_BAR", &out);
  ASSERT_EQ("FooBar", out);


  AllCapsToCamelCase("FOO-BAR", &out);
  ASSERT_EQ("FooBar", out);

  AllCapsToCamelCase("FOOBAR", &out);
  ASSERT_EQ("Foobar", out);
}

TEST(TestStringCase, TestToUpperCase) {
  string out;
  ToUpperCase(string("foo"), &out);
  ASSERT_EQ("FOO", out);
  ToUpperCase(string("foo bar-BaZ"), &out);
  ASSERT_EQ("FOO BAR-BAZ", out);
}

TEST(TestStringCase, TestToUpperCaseInPlace) {
  string in_out = "foo";
  ToUpperCase(in_out, &in_out);
  ASSERT_EQ("FOO", in_out);
}

TEST(TestStringCase, TestCapitalize) {
  string word = "foo";
  Capitalize(&word);
  ASSERT_EQ("Foo", word);

  word = "HiBerNATe";
  Capitalize(&word);
  ASSERT_EQ("Hibernate", word);
}

} // namespace yb
