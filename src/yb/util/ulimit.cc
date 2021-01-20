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

#include "yb/util/ulimit.h"

#include <sys/resource.h>

namespace yb {

ResourceLimit::ResourceLimit(int64_t value) : value_(value) {}

std::string ResourceLimit::ToString() const {
  if (IsUnlimited()) return "unlimited";
  return std::to_string(value_);
}

bool ResourceLimit::IsNegative() const {
  return !IsUnlimited() && value_ < 0;
}

bool ResourceLimit::IsUnlimited() const {
  return value_ == RLIM_INFINITY;
}

bool ResourceLimit::operator==(const ResourceLimit& other) const {
  return value_ == other.value_;
}

bool ResourceLimit::operator<(const ResourceLimit& other) const {
  if (value_ == RLIM_INFINITY) return false;
  if (other.value_  == RLIM_INFINITY) return true;
  return value_ < other.value_;
}

bool ResourceLimit::operator<=(const ResourceLimit& other) const {
  return !(other < *this);
}

int64_t ResourceLimit::RawValue() const {
  return value_;
}

} // namespace yb
