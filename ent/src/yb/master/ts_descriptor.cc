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

#include "yb/master/ts_descriptor.h"
#include "yb/master/master.pb.h"

using std::set;

namespace yb {
namespace master {
namespace enterprise {

bool TSDescriptor::IsReadOnlyTS(const ReplicationInfoPB& replication_info) const {
  const PlacementInfoPB& placement_info = replication_info.live_replicas();
  if (placement_info.has_placement_uuid()) {
    return placement_info.placement_uuid() != placement_uuid();
  }
  return !placement_uuid().empty();
}

bool TSDescriptor::IsAcceptingLeaderLoad(const ReplicationInfoPB& replication_info) const {
  if (IsReadOnlyTS(replication_info)) {
    // Read-only ts are not voting and therefore cannot be leaders.
    return false;
  }

  if (replication_info.affinitized_leaders_size() == 0) {
    // If there are no affinitized leaders, all ts can be leaders.
    return true;
  }

  for (const CloudInfoPB& cloud_info : replication_info.affinitized_leaders()) {
    if (MatchesCloudInfo(cloud_info)) {
      return true;
    }
  }
  return false;
}

} // namespace enterprise
} // namespace master
} // namespace yb
