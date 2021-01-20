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

#include "yb/integration-tests/load_balancer_test_util.h"

namespace yb {
namespace integration_tests {

bool AreLoadsBalanced(const std::vector<uint32_t>& tserver_loads) {
    if (tserver_loads.empty()) {
      return true;
    }
    int min_load = tserver_loads[0];
    int max_load = min_load;
    for (int i = 1; i < tserver_loads.size(); ++i) {
      if (tserver_loads[i] < min_load) {
        min_load = tserver_loads[i];
      } else if (tserver_loads[i] > max_load) {
        max_load = tserver_loads[i];
      }
    }
    return (max_load - min_load) < 2;
}

}  // namespace integration_tests
}  // namespace yb
