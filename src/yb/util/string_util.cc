//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
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

#include "yb/util/string_util.h"

#include <regex>
#include <boost/algorithm/string/predicate.hpp>

#include "yb/util/logging.h"

using std::vector;
using std::regex;
using std::regex_match;
using std::string;
using std::stringstream;
using boost::algorithm::iequals;

namespace yb {

bool IsBigInteger(const Slice& s) {
  static const regex int_regex("[+-]?[0-9]+");
  return regex_match(s.cdata(), int_regex);
}

bool IsDecimal(const Slice& s) {
  // Regexes are based (but do not match exactly) the definition of Decimal::FromString
  static const string optional_exp_suffix = "([eE][+-]?[0-9]+)?";
  static const regex decimal_regex_1("[+-]?[0-9]*\\.[0-9]+" + optional_exp_suffix);
  static const regex decimal_regex_2("[+-]?[0-9]+\\.?" + optional_exp_suffix);
  return IsBigInteger(s)
      || regex_match(s.cdata(), decimal_regex_1)
      || regex_match(s.cdata(), decimal_regex_2);
}

bool IsBoolean(const Slice& s) {
  return iequals(s.cdata(), "true") || iequals(s.cdata(), "false");
}

vector<string> StringSplit(const string& arg, char delim) {
  vector<string> splits;
  stringstream ss(arg);
  string item;
  while (getline(ss, item, delim)) {
    splits.push_back(std::move(item));
  }
  return splits;
}

bool StringEndsWith(const string& s, const char* end, size_t end_len, string* left) {
  // For our purpose, s should always have at least one character before the string we are looking
  // for.
  if (s.length() <= end_len) {
    return false;
  }
  if (s.find(end, s.length() - end_len) != string::npos) {
    if (left != nullptr) {
      *left = s.substr(0, s.length() - end_len);
    }
    return true;
  }
  return false;
}

void AppendWithSeparator(const string& to_append, string* dest, const char* separator) {
  CHECK_NOTNULL(dest);
  if (!dest->empty()) {
    *dest += separator;
  }
  *dest += to_append;
}

void AppendWithSeparator(const char* to_append, string* dest, const char* separator) {
  CHECK_NOTNULL(dest);
  if (!dest->empty()) {
    *dest += separator;
  }
  *dest += to_append;
}

}  // namespace yb
