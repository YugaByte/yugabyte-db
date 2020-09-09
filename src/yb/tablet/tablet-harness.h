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
#ifndef YB_TABLET_TABLET_HARNESS_H
#define YB_TABLET_TABLET_HARNESS_H

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "yb/common/partition.h"
#include "yb/common/schema.h"
#include "yb/consensus/log_anchor_registry.h"
#include "yb/fs/fs_manager.h"
#include "yb/server/logical_clock.h"
#include "yb/server/metadata.h"

#include "yb/tablet/tablet_fwd.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/tablet_options.h"
#include "yb/util/env.h"
#include "yb/util/mem_tracker.h"
#include "yb/util/metrics.h"
#include "yb/util/status.h"

using std::string;
using std::vector;

namespace yb {
namespace tablet {

// Creates a default partition schema and partition for a table.
//
// The provided schema must include column IDs.
//
// The partition schema will have no hash components, and a single range component over the primary
// key columns. The partition will cover the entire partition-key space.
static std::pair<PartitionSchema, Partition> CreateDefaultPartition(const Schema& schema) {
  // Create a default partition schema.
  PartitionSchema partition_schema;
  CHECK_OK(PartitionSchema::FromPB(PartitionSchemaPB(), schema, &partition_schema));

  // Create the tablet partitions.
  vector<Partition> partitions;
  CHECK_OK(partition_schema.CreatePartitions(vector<YBPartialRow>(), schema, &partitions));
  CHECK_EQ(1, partitions.size());
  return std::make_pair(partition_schema, partitions[0]);
}

class TabletHarness {
 public:
  struct Options {
    explicit Options(string root_dir)
        : env(Env::Default()),
          tablet_id("test_tablet_id"),
          root_dir(std::move(root_dir)),
          table_type(TableType::DEFAULT_TABLE_TYPE),
          enable_metrics(true) {}

    Env* env;
    string tablet_id;
    string root_dir;
    TableType table_type;
    bool enable_metrics;
  };

  TabletHarness(const Schema& schema, Options options)
      : options_(std::move(options)), schema_(schema) {}

  virtual ~TabletHarness() {}

  CHECKED_STATUS Create(bool first_time) {
    std::pair<PartitionSchema, Partition> partition(CreateDefaultPartition(schema_));

    // Build the Tablet
    fs_manager_.reset(new FsManager(options_.env, options_.root_dir, "tserver_test"));
    if (first_time) {
      RETURN_NOT_OK(fs_manager_->CreateInitialFileSystemLayout());
    }
    RETURN_NOT_OK(fs_manager_->Open());

    RaftGroupMetadataPtr metadata;
    RETURN_NOT_OK(RaftGroupMetadata::LoadOrCreate(
        fs_manager_.get(),
        "YBTableTest",
        options_.tablet_id,
        "test",
        "YBTableTest",
        options_.table_type,
        schema_,
        partition.first,
        partition.second,
        boost::none /* index_info */,
        TABLET_DATA_READY,
        &metadata));
    if (options_.enable_metrics) {
      metrics_registry_.reset(new MetricRegistry());
    }

    clock_ = server::LogicalClock::CreateStartingAt(HybridTime::kInitial);
    tablet_ = std::make_shared<Tablet>(MakeTabletInitData(metadata));
    return Status::OK();
  }

  CHECKED_STATUS Open() {
    RETURN_NOT_OK(tablet_->Open());
    tablet_->MarkFinishedBootstrapping();
    return tablet_->EnableCompactions(/* operation_pause */ nullptr);
  }

  Result<TabletPtr> OpenTablet(const TabletId& tablet_id) {
    RaftGroupMetadataPtr metadata;
    RETURN_NOT_OK(RaftGroupMetadata::Load(fs_manager_.get(), tablet_id, &metadata));
    TabletOptions tablet_options;
    auto tablet = std::make_shared<Tablet>(MakeTabletInitData(metadata));
    RETURN_NOT_OK(tablet->Open());
    tablet->MarkFinishedBootstrapping();
    RETURN_NOT_OK(tablet->EnableCompactions(/* operation_pause */ nullptr));
    return tablet;
  }

  TabletInitData MakeTabletInitData(const RaftGroupMetadataPtr& metadata) {
    return TabletInitData {
      .metadata = metadata,
      .client_future = std::shared_future<client::YBClient*>(),
      .clock = clock_,
      .parent_mem_tracker = std::shared_ptr<MemTracker>(),
      .block_based_table_mem_tracker = std::shared_ptr<MemTracker>(),
      .metric_registry = metrics_registry_.get(),
      .log_anchor_registry = new log::LogAnchorRegistry(),
      .tablet_options = TabletOptions(),
      .log_prefix_suffix = std::string(),
      .transaction_participant_context = nullptr,
      .local_tablet_filter = client::LocalTabletFilter(),
      .transaction_coordinator_context = nullptr,
      .txns_enabled = TransactionsEnabled::kFalse,
      .is_sys_catalog = IsSysCatalogTablet::kFalse,
      .snapshot_coordinator = nullptr,
      .tablet_splitter = nullptr
    };
  }

  server::Clock* clock() const {
    return clock_.get();
  }

  const TabletPtr& tablet() {
    return tablet_;
  }

  FsManager* fs_manager() {
    return fs_manager_.get();
  }

  MetricRegistry* metrics_registry() {
    return metrics_registry_.get();
  }

  const Options& options() const { return options_; }

 private:
  Options options_;

  gscoped_ptr<MetricRegistry> metrics_registry_;

  scoped_refptr<server::Clock> clock_;
  Schema schema_;
  gscoped_ptr<FsManager> fs_manager_;
  TabletPtr tablet_;
};

} // namespace tablet
} // namespace yb
#endif // YB_TABLET_TABLET_HARNESS_H
