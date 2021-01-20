//
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
//

#include "yb/client/tablet_rpc.h"
#include "yb/client/meta_cache.h"

#include "yb/util/test_util.h"

namespace yb {
namespace client {
namespace internal {

const TabletId kTestTablet = "kTestTablet";

class TabletRpcTest : public YBTest {
 public:
  TabletRpcTest() {
    cloud_info_.set_placement_cloud("cloud1");
    cloud_info_.set_placement_region("datacenter1");
    cloud_info_.set_placement_zone("rack1");
  }

  void FillTsInfo(
      const std::string& uuid, const std::string& host, const std::string& addr,
      master::TSInfoPB* ts_info) {
    ts_info->set_permanent_uuid(uuid);
    ts_info->mutable_cloud_info()->CopyFrom(cloud_info_);
    ts_info->set_placement_uuid("");
    auto* rpc_addr = ts_info->add_private_rpc_addresses();
    rpc_addr->set_host(addr);
    rpc_addr->set_port(9100);
  }

 private:
  CloudInfoPB cloud_info_;
};

TEST_F(TabletRpcTest, TabletInvokerSelectTabletServerRace) {

  master::TabletLocationsPB tablet_locations;
  tablet_locations.set_tablet_id(kTestTablet);
  tablet_locations.set_stale(false);

  master::TabletLocationsPB_ReplicaPB replica1;
  FillTsInfo("n1-uuid", "n1", "127.0.0.1", replica1.mutable_ts_info());

  master::TabletLocationsPB_ReplicaPB replica2;
  FillTsInfo("n2-uuid", "n2", "127.0.0.2", replica2.mutable_ts_info());

  TabletServerMap ts_map;

  for (auto* replica : {&replica1, &replica2}) {
    replica->set_role(consensus::RaftPeerPB_Role::RaftPeerPB_Role_FOLLOWER);
    replica->set_member_type(consensus::RaftPeerPB_MemberType::RaftPeerPB_MemberType_VOTER);

    const auto& uuid = replica->ts_info().permanent_uuid();
    ts_map.emplace(uuid, std::make_unique<RemoteTabletServer>(uuid, nullptr, nullptr));
  }

  Partition partition;
  Partition::FromPB(tablet_locations.partition(), &partition);
  internal::RemoteTabletPtr remote_tablet = new internal::RemoteTablet(
      tablet_locations.tablet_id(), partition, 0 /* split_depth */, "" /* split_parent_id */);

  std::atomic<bool> stop_requested{false};
  std::thread replicas_refresher(
      [&stop_requested, &remote_tablet, &ts_map, &tablet_locations, &replica1, &replica2]{
    bool two_replicas = false;
    while (!stop_requested) {
      tablet_locations.clear_replicas();
      if (two_replicas) {
        tablet_locations.add_replicas()->CopyFrom(replica1);
      }
      tablet_locations.add_replicas()->CopyFrom(replica2);
      remote_tablet->Refresh(ts_map, tablet_locations.replicas());
      two_replicas = !two_replicas;
    }
  });

  scoped_refptr<Trace> trace(new Trace());

  for (int iter = 0; iter < 200; ++iter) {
    internal::TabletInvoker invoker(false /* local_tserver_only */,
                                    false /* consistent_prefix */,
                                    nullptr /* client */,
                                    nullptr /* command */,
                                    nullptr /* rpc */,
                                    remote_tablet.get(),
                                    /* table =*/ nullptr,
                                    nullptr /* retrier */,
                                    trace.get());
    invoker.SelectTabletServer();
  }

  stop_requested = true;
  replicas_refresher.join();
}

} // namespace internal
} // namespace client
} // namespace yb
