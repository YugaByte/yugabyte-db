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
#include "yb/tablet/tablet_bootstrap_if.h"

#include "yb/consensus/log_anchor_registry.h"
#include "yb/tablet/tablet_bootstrap.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/util/debug/trace_event.h"

namespace yb {
namespace tablet {

using std::shared_ptr;

using consensus::ConsensusBootstrapInfo;

TabletStatusListener::TabletStatusListener(const RaftGroupMetadataPtr& meta)
    : meta_(meta) {
}

const string TabletStatusListener::tablet_id() const {
  return meta_->raft_group_id();
}

const string TabletStatusListener::namespace_name() const {
  return meta_->namespace_name();
}

const string TabletStatusListener::table_name() const {
  return meta_->table_name();
}

const string TabletStatusListener::table_id() const {
  return meta_->table_id();
}

std::shared_ptr<Partition> TabletStatusListener::partition() const {
  return meta_->partition();
}

SchemaPtr TabletStatusListener::schema() const {
  return meta_->schema();
}

TabletStatusListener::~TabletStatusListener() {
}

void TabletStatusListener::StatusMessage(const string& status) {
  LOG(INFO) << "T " << tablet_id() << " P " << meta_->fs_manager()->uuid() << ": "
            << status;
  std::lock_guard<boost::shared_mutex> l(lock_);
  last_status_ = status;
}

Status BootstrapTablet(
    const BootstrapTabletData& data,
    TabletPtr* rebuilt_tablet,
    scoped_refptr<log::Log>* rebuilt_log,
    ConsensusBootstrapInfo* consensus_info) {
  const auto& meta = *data.tablet_init_data.metadata;
  TRACE_EVENT1("tablet", "BootstrapTablet", "tablet_id", meta.raft_group_id());
  RETURN_NOT_OK(BootstrapTabletImpl(data, rebuilt_tablet, rebuilt_log, consensus_info));

  // Set WAL retention time from the metadata.
  (*rebuilt_log)->set_wal_retention_secs(meta.wal_retention_secs());
  (*rebuilt_log)->set_cdc_min_replicated_index(meta.cdc_min_replicated_index());

  // This is necessary since OpenNewLog() initially disables sync.
  RETURN_NOT_OK((*rebuilt_log)->ReEnableSyncIfRequired());
  return Status::OK();
}

string DocDbOpIds::ToString() const {
  return Format("{ regular: $0 intents: $1 }", regular, intents);
}

} // namespace tablet
} // namespace yb
