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

#include "yb/tserver/tablet_split_heartbeat_data_provider.h"

#include "yb/master/master.pb.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tserver/service_util.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/util/logging.h"

DEFINE_int32(tablet_split_monitor_heartbeat_interval_ms, 5000,
             "Interval (in milliseconds) at which tserver check tablets and sends a list of "
             "tablets to split in a heartbeat to master.");

using namespace std::literals;

namespace yb {
namespace tserver {

TabletSplitHeartbeatDataProvider::TabletSplitHeartbeatDataProvider(TabletServer* server) :
  PeriodicalHeartbeatDataProvider(server,
      MonoDelta::FromMilliseconds(FLAGS_tablet_split_monitor_heartbeat_interval_ms)) {}

void TabletSplitHeartbeatDataProvider::DoAddData(
    const master::TSHeartbeatResponsePB& last_resp, master::TSHeartbeatRequestPB* req) {
  if (!last_resp.has_tablet_split_size_threshold_bytes()) {
    return;
  }
  const auto split_size_threshold = last_resp.tablet_split_size_threshold_bytes();
  VLOG_WITH_FUNC(2) << "split_size_threshold: " << split_size_threshold;
  if (split_size_threshold <= 0) {
    return;
  }

  const auto tablet_peers = server().tablet_manager()->GetTabletPeers();

  for (const auto& tablet_peer : tablet_peers) {
    if (!tablet_peer->CheckRunning().ok() || !LeaderTerm(*tablet_peer).ok()) {
      // Only check tablets for which current tserver is leader.
      VLOG_WITH_FUNC(3) << Format("Skipping tablet: $0 (non leader)", tablet_peer->tablet_id());
      continue;
    }
    const auto& tablet = tablet_peer->shared_tablet();
    if (!tablet || !tablet->metadata() ||
        tablet->table_type() == TableType::TRANSACTION_STATUS_TABLE_TYPE ||
        // TODO(tsplit): Tablet splitting for colocated tables is not supported.
        tablet->metadata()->colocated() ||
        tablet->metadata()->tablet_data_state() != tablet::TabletDataState::TABLET_DATA_READY ||
        tablet->GetCurrentVersionSstFilesSize() < split_size_threshold ||
        // TODO(tsplit): We don't split not yet fully compacted post-split tablets for now, since
        // detecting effective middle key and tablet size for such tablets is not yet implemented.
        tablet->StillHasParentDataAfterSplit()) {
      VLOG_WITH_FUNC(3) << Format(
          "Skipping tablet: $0, data state: $1, SST files size: $2, has key bounds: $3, has been "
          "fully compacted: $4",
          tablet->tablet_id(),
          tablet->metadata() ? AsString(tablet->metadata()->tablet_data_state()) : "NONE",
          tablet->GetCurrentVersionSstFilesSize(), tablet->doc_db().key_bounds->IsInitialized(),
          tablet->metadata()->has_been_fully_compacted());
      continue;
    }

    const auto& tablet_id = tablet->tablet_id();
    const auto split_encoded_key = tablet->GetEncodedMiddleSplitKey();
    if (!split_encoded_key.ok()) {
      LOG(WARNING) << Format(
          "Failed to get middle split key for tablet $0: $1", tablet_id,
          split_encoded_key.status());
      continue;
    }

    const auto doc_key_hash = docdb::DecodeDocKeyHash(*split_encoded_key);
    if (!doc_key_hash.ok()) {
      LOG(ERROR) << Format(
          "Failed to decode hash code from the middle split key for tablet $0: $1", tablet_id,
          doc_key_hash.status());
      continue;
    }

    auto* const tablet_for_split = req->add_tablets_for_split();
    tablet_for_split->set_tablet_id(tablet_id);
    tablet_for_split->set_split_encoded_key(*split_encoded_key);
    if (doc_key_hash->has_value()) {
      tablet_for_split->set_split_partition_key(
          PartitionSchema::EncodeMultiColumnHashValue(doc_key_hash->value()));
    } else {
      tablet_for_split->set_split_partition_key(*split_encoded_key);
    }
    VLOG_WITH_FUNC(1) << Format(
        "Found tablet to split: $0, size: $1", tablet->tablet_id(),
        tablet->GetCurrentVersionSstFilesSize());
    // TODO(tsplit): remove this return after issue with splitting more than one tablet "at once"
    // is fixed.
    return;
  }
}

} // namespace tserver
} // namespace yb
