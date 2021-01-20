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

#ifndef YB_UTIL_CAPABILITIES_H
#define YB_UTIL_CAPABILITIES_H

#include <cstdint>
#include <vector>

namespace yb {

typedef uint32_t CapabilityId;

class CapabilityRegisterer {
 public:
  explicit CapabilityRegisterer(CapabilityId capability);
};

std::vector<CapabilityId> Capabilities();

#define DEFINE_CAPABILITY(name, id) \
  namespace capabilities {                                                                         \
    yb::CapabilityId BOOST_PP_CAT(CAPABILITY_, name) = id;                                         \
    static yb::CapabilityRegisterer BOOST_PP_CAT(CAP_REG_, name)(BOOST_PP_CAT(CAPABILITY_, name)); \
  }                                                                                                \
  using capabilities::BOOST_PP_CAT(CAPABILITY_, name);                                             \
  /**/

#define DECLARE_CAPABILITY(name) \
  namespace capabilities { extern yb::CapabilityId BOOST_PP_CAT(CAPABILITY_, name); } \
  using capabilities::BOOST_PP_CAT(CAPABILITY_, name);

} // namespace yb

#endif // YB_UTIL_CAPABILITIES_H
