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

#include "yb/tablet/operations/change_metadata_operation.h"

#include <glog/logging.h>

#include "yb/common/wire_protocol.h"
#include "yb/consensus/consensus.h"
#include "yb/rpc/rpc_context.h"
#include "yb/server/hybrid_clock.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/tablet_metrics.h"
#include "yb/tserver/tserver.pb.h"

#include "yb/util/scope_exit.h"
#include "yb/util/trace.h"

namespace yb {
namespace tablet {

using std::bind;
using consensus::ReplicateMsg;
using consensus::CHANGE_METADATA_OP;
using consensus::DriverType;
using google::protobuf::RepeatedPtrField;
using strings::Substitute;
using tserver::TabletServerErrorPB;
using tserver::ChangeMetadataRequestPB;
using tserver::ChangeMetadataResponsePB;

void ChangeMetadataOperationState::SetIndexes(const RepeatedPtrField<IndexInfoPB>& indexes) {
  index_map_.FromPB(indexes);
}

string ChangeMetadataOperationState::ToString() const {
  return Format("ChangeMetadataOperationState { hybrid_time: $0 schema: $1 request: $2 }",
                hybrid_time_even_if_unset(), schema_, request());
}

void ChangeMetadataOperationState::UpdateRequestFromConsensusRound() {
  UseRequest(consensus_round()->replicate_msg()->mutable_change_metadata_request());
}

ChangeMetadataOperation::ChangeMetadataOperation(
    std::unique_ptr<ChangeMetadataOperationState> state)
    : Operation(std::move(state), OperationType::kChangeMetadata) {}

consensus::ReplicateMsgPtr ChangeMetadataOperation::NewReplicateMsg() {
  auto result = std::make_shared<ReplicateMsg>();
  result->set_op_type(CHANGE_METADATA_OP);
  result->mutable_change_metadata_request()->CopyFrom(*state()->request());
  return result;
}

Status ChangeMetadataOperation::Prepare() {
  TRACE("PREPARE CHANGE-METADATA: Starting");

  // Decode schema
  auto has_schema = state()->request()->has_schema();
  std::unique_ptr<Schema> schema;
  if (has_schema) {
    schema = std::make_unique<Schema>();
    Status s = SchemaFromPB(state()->request()->schema(), schema.get());
    if (!s.ok()) {
      state()->SetError(s, TabletServerErrorPB::INVALID_SCHEMA);
      return s;
    }
  }

  Tablet* tablet = state()->tablet();
  RETURN_NOT_OK(tablet->CreatePreparedChangeMetadata(state(), schema.get()));

  if (has_schema) {
    state()->AddToAutoReleasePool(schema.release());
  }

  state()->SetIndexes(state()->request()->indexes());

  TRACE("PREPARE CHANGE-METADATA: finished");
  return Status::OK();
}

void ChangeMetadataOperation::DoStart() {
  state()->TrySetHybridTimeFromClock();
  TRACE("START. HybridTime: $0",
      server::HybridClock::GetPhysicalValueMicros(state()->hybrid_time()));
}

Status ChangeMetadataOperation::DoReplicated(int64_t leader_term, Status* complete_status) {
  TRACE("APPLY CHANGE-METADATA: Starting");

  auto se = ScopeExit([this] {
    state()->Finish();
  });

  Tablet* tablet = state()->tablet();
  log::Log* log = state()->mutable_log();
  size_t num_operations = 0;

  if (state()->request()->has_wal_retention_secs()) {
    // We don't consider wal retention changes as another operation because this value is always
    // sent together with the schema, as long as it has been changed in the master's sys-catalog.
    auto s = tablet->AlterWalRetentionSecs(state());
    if (s.ok()) {
      log->set_wal_retention_secs(state()->request()->wal_retention_secs());
    } else {
      LOG(WARNING) << "T " << tablet->tablet_id() << ": Unable to alter wal retention secs";
    }
  }

  // Only perform one operation.
  enum MetadataChange {
    NONE,
    SCHEMA,
    ADD_TABLE,
    REMOVE_TABLE,
    BACKFILL_DONE,
  };

  MetadataChange metadata_change = MetadataChange::NONE;
  bool request_has_newer_schema = false;
  if (state()->request()->has_schema()) {
    metadata_change = MetadataChange::SCHEMA;
    request_has_newer_schema = tablet->metadata()->schema_version() < state()->schema_version();
    if (request_has_newer_schema) {
      ++num_operations;
    }
  }

  if (state()->request()->has_add_table()) {
    metadata_change = MetadataChange::NONE;
    if (++num_operations == 1) {
      metadata_change = MetadataChange::ADD_TABLE;
    }
  }

  if (state()->request()->has_remove_table_id()) {
    metadata_change = MetadataChange::NONE;
    if (++num_operations == 1) {
      metadata_change = MetadataChange::REMOVE_TABLE;
    }
  }

  if (state()->request()->has_mark_backfill_done()) {
    metadata_change = MetadataChange::NONE;
    if (++num_operations == 1) {
      metadata_change = MetadataChange::BACKFILL_DONE;
    }
  }

  switch (metadata_change) {
    case MetadataChange::NONE:
      return STATUS_FORMAT(
          InvalidArgument, "Wrong number of operations in Change Metadata Operation: $0",
          num_operations);
    case MetadataChange::SCHEMA:
      if (!request_has_newer_schema) {
        LOG_WITH_PREFIX(INFO)
            << "Already running schema version " << tablet->metadata()->schema_version()
            << " got alter request for version " << state()->schema_version();
        break;
      }
      DCHECK_EQ(1, num_operations) << "Invalid number of change metadata operations: "
                                   << num_operations;
      RETURN_NOT_OK(tablet->AlterSchema(state()));
      log->SetSchemaForNextLogSegment(*DCHECK_NOTNULL(state()->schema()),
                                      state()->schema_version());
      break;
    case MetadataChange::ADD_TABLE:
      DCHECK_EQ(1, num_operations) << "Invalid number of change metadata operations: "
                                   << num_operations;
      RETURN_NOT_OK(tablet->AddTable(state()->request()->add_table()));
      break;
    case MetadataChange::REMOVE_TABLE:
      DCHECK_EQ(1, num_operations) << "Invalid number of change metadata operations: "
                                   << num_operations;
      RETURN_NOT_OK(tablet->RemoveTable(state()->request()->remove_table_id()));
      break;
    case MetadataChange::BACKFILL_DONE:
      DCHECK_EQ(1, num_operations) << "Invalid number of change metadata operations: "
                                   << num_operations;
      RETURN_NOT_OK(tablet->MarkBackfillDone());
      break;
  }

  // Now that all of the changes have been applied and the commit is durable
  // make the changes visible to readers.
  TRACE("AlterSchemaCommitCallback: making alter schema visible");
  return Status::OK();
}

Status ChangeMetadataOperation::DoAborted(const Status& status) {
  TRACE("AlterSchemaCommitCallback: transaction aborted");
  state()->Finish();
  return status;
}

string ChangeMetadataOperation::ToString() const {
  return Format("ChangeMetadataOperation { state: $0 }", state());
}

CHECKED_STATUS SyncReplicateChangeMetadataOperation(
    const tserver::ChangeMetadataRequestPB* req,
    tablet::TabletPeer* tablet_peer,
    int64_t term) {
  auto operation_state = std::make_unique<ChangeMetadataOperationState>(
      tablet_peer->tablet(), tablet_peer->log(), req);

  Synchronizer synchronizer;

  operation_state->set_completion_callback(
      std::make_unique<tablet::SynchronizerOperationCompletionCallback>(&synchronizer));

  tablet_peer->Submit(std::make_unique<tablet::ChangeMetadataOperation>(
      std::move(operation_state)), term);

  return synchronizer.Wait();
}

}  // namespace tablet
}  // namespace yb
