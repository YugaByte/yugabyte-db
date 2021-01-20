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
#include <cctype>
#include <algorithm>
#include <glog/logging.h>

#include "yb/util/string_case.h"

namespace yb {

using std::string;

void SnakeToCamelCase(const std::string &snake_case,
                      std::string *camel_case) {
  DCHECK_NE(camel_case, &snake_case) << "Does not support in-place operation";
  camel_case->clear();
  camel_case->reserve(snake_case.size());

  bool uppercase_next = true;
  for (char c : snake_case) {
    if ((c == '_') ||
        (c == '-')) {
      uppercase_next = true;
      continue;
    }
    if (uppercase_next) {
      camel_case->push_back(toupper(c));
    } else {
      camel_case->push_back(c);
    }
    uppercase_next = false;
  }
}

void AllCapsToCamelCase(const std::string &all_caps,
                        std::string *camel_case) {
  DCHECK_NE(camel_case, &all_caps) << "Does not support in-place operation";
  camel_case->clear();
  camel_case->reserve(all_caps.size());

  bool uppercase_next = true;
  for (char c : all_caps) {
    if ((c == '_') ||
        (c == '-')) {
      uppercase_next = true;
      continue;
    }
    if (uppercase_next) {
      camel_case->push_back(c);
    } else {
      camel_case->push_back(tolower(c));
    }
    uppercase_next = false;
  }
}

std::string AllCapsToCamelCase(const std::string &all_caps) {
  std::string sresult;
  AllCapsToCamelCase(all_caps, &sresult);
  return sresult;
}

void ToLowerCase(const std::string &string,
                 std::string *out) {
  if (out != &string) {
    *out = string;
  }

  for (char& c : *out) {
    c = tolower(c);
  }
}

void ToUpperCase(const std::string &string,
                 std::string *out) {
  if (out != &string) {
    *out = string;
  }

  for (char& c : *out) {
    c = toupper(c);
  }
}

void Capitalize(string *word) {
  uint32_t size = word->size();
  if (size == 0) {
    return;
  }

  (*word)[0] = toupper((*word)[0]);

  for (int i = 1; i < size; i++) {
    (*word)[i] = tolower((*word)[i]);
  }
}

bool ContainsUpperCase(const string& str) {
  return std::any_of(str.begin(), str.end(), isupper);
}

}  // namespace yb
