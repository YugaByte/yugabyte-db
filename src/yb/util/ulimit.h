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


#ifndef YB_UTIL_ULIMIT_H
#define YB_UTIL_ULIMIT_H

#include <string>

namespace yb {

// Represents a resource limit value, in particular abstracting away some of the system-specific
// idiosyncratic representations of "unlimited" values.
class ResourceLimit {
 public:
  explicit ResourceLimit(int64_t value);

  // Return a string representation, which is just the stringified numeric value, or "unlimited".
  std::string ToString() const;

  // Return true i.f.f. this represents a negative value.
  bool IsNegative() const;

  // Return true i.f.f. this represents an unlimited value;
  bool IsUnlimited() const;

  bool operator==(const ResourceLimit& other) const;

  bool operator<(const ResourceLimit& other) const;

  bool operator<=(const ResourceLimit& other) const;

  int64_t RawValue() const;

 private:
  int64_t value_;
};

struct ResourceLimits {
  ResourceLimit soft;
  ResourceLimit hard;
};

} // namespace yb

#endif // YB_UTIL_ULIMIT_H
