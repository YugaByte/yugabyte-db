// Copyright (c) YugaByte, Inc.

#include <glog/logging.h>

#include "yb/tablet/operations/snapshot_operation.h"

#include "yb/common/snapshot.h"
#include "yb/common/wire_protocol.h"
#include "yb/consensus/consensus.h"
#include "yb/rpc/rpc_context.h"
#include "yb/server/hybrid_clock.h"
#include "yb/tablet/snapshot_coordinator.h"
#include "yb/tablet/tablet_snapshots.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/tablet_metrics.h"
#include "yb/tserver/backup.pb.h"
#include "yb/tserver/tserver_error.h"
#include "yb/util/trace.h"

namespace yb {
namespace tablet {

using std::bind;
using std::string;
using consensus::ReplicateMsg;
using consensus::SNAPSHOT_OP;
using consensus::DriverType;
using strings::Substitute;
using yb::tserver::TabletServerError;
using yb::tserver::TabletServerErrorPB;
using yb::tserver::TabletSnapshotOpRequestPB;

// ------------------------------------------------------------------------------------------------
// SnapshotOperationState
// ------------------------------------------------------------------------------------------------

string SnapshotOperationState::ToString() const {
  return Format("SnapshotOperationState { hybrid_time: $0 request: $1 }",
                hybrid_time(), request());
}

std::string SnapshotOperationState::GetSnapshotDir(const string& top_snapshots_dir) const {
  auto& request = *this->request();
  if (!request.snapshot_dir_override().empty()) {
    return request.snapshot_dir_override();
  }
  std::string snapshot_id_str;
  auto txn_snapshot_id = TryFullyDecodeTxnSnapshotId(request.snapshot_id());
  if (txn_snapshot_id) {
    snapshot_id_str = txn_snapshot_id.ToString();
  } else {
    snapshot_id_str = request.snapshot_id();
  }

  return JoinPathSegments(top_snapshots_dir, snapshot_id_str);
}

bool SnapshotOperationState::CheckOperationRequirements() {
  if (operation() == TabletSnapshotOpRequestPB::RESTORE) {
    const string top_snapshots_dir = tablet()->metadata()->snapshots_dir();
    const string snapshot_dir = GetSnapshotDir(top_snapshots_dir);
    Status s = tablet()->rocksdb_env().FileExists(snapshot_dir);

    if (!s.ok()) {
      s = s.CloneAndAddErrorCode(TabletServerError(TabletServerErrorPB::INVALID_SNAPSHOT));
      // LogPrefix() calls ToString() which needs correct hybrid_time.
      TrySetHybridTimeFromClock();
      LOG_WITH_PREFIX(WARNING)
          << Format("Snapshot directory does not exist: $0 $1", s, snapshot_dir);
      TRACE("Requirements was not satisfied for snapshot operation: $0", operation());
      // Run the callback, finish RPC and return the error to the sender.
      CompleteWithStatus(s);
      Finish();
      return false;
    }
  }

  return true;
}

Result<SnapshotCoordinator&> GetSnapshotCoordinator(SnapshotOperationState* state) {
  auto snapshot_coordinator = state->tablet()->snapshot_coordinator();
  if (!snapshot_coordinator) {
    return STATUS_FORMAT(IllegalState, "Replicated $0 to tablet without snapshot coordinator",
                         TabletSnapshotOpRequestPB::Operation_Name(state->request()->operation()));
  }
  return *snapshot_coordinator;
}

Status SnapshotOperationState::Apply(int64_t leader_term) {
  TRACE("APPLY SNAPSHOT: Starting");
  auto operation = request()->operation();
  switch (operation) {
    case TabletSnapshotOpRequestPB::CREATE_ON_MASTER:
      return VERIFY_RESULT(GetSnapshotCoordinator(this)).get().CreateReplicated(leader_term, *this);
    case TabletSnapshotOpRequestPB::DELETE_ON_MASTER:
      return VERIFY_RESULT(GetSnapshotCoordinator(this)).get().DeleteReplicated(leader_term, *this);
    case TabletSnapshotOpRequestPB::CREATE_ON_TABLET:
      return tablet()->snapshots().Create(this);
    case TabletSnapshotOpRequestPB::RESTORE:
      return tablet()->snapshots().Restore(this);
    case TabletSnapshotOpRequestPB::DELETE_ON_TABLET:
      return tablet()->snapshots().Delete(this);
    case google::protobuf::kint32min: FALLTHROUGH_INTENDED;
    case google::protobuf::kint32max: FALLTHROUGH_INTENDED;
    case TabletSnapshotOpRequestPB::UNKNOWN:
      break;
  }
  FATAL_INVALID_ENUM_VALUE(TabletSnapshotOpRequestPB::Operation, operation);
}

// ------------------------------------------------------------------------------------------------
// SnapshotOperation
// ------------------------------------------------------------------------------------------------

SnapshotOperation::SnapshotOperation(std::unique_ptr<SnapshotOperationState> state)
    : Operation(std::move(state), OperationType::kSnapshot) {}

void SnapshotOperationState::UpdateRequestFromConsensusRound() {
  UseRequest(consensus_round()->replicate_msg()->mutable_snapshot_request());
}

consensus::ReplicateMsgPtr SnapshotOperation::NewReplicateMsg() {
  auto result = std::make_shared<ReplicateMsg>();
  result->set_op_type(SNAPSHOT_OP);
  auto request = state()->ReleaseRequest();
  if (request) {
    result->set_allocated_snapshot_request(request);
  } else {
    *result->mutable_snapshot_request() = *state()->request();
  }
  return result;
}

Status SnapshotOperation::Prepare() {
  TRACE("PREPARE SNAPSHOT: Starting");
  RETURN_NOT_OK(state()->tablet()->snapshots().Prepare(this));

  TRACE("PREPARE SNAPSHOT: finished");
  return Status::OK();
}

void SnapshotOperation::DoStart() {
  state()->TrySetHybridTimeFromClock();

  TRACE("START. HybridTime: $0",
      server::HybridClock::GetPhysicalValueMicros(state()->hybrid_time()));
}

Status SnapshotOperation::DoAborted(const Status& status) {
  TRACE("SnapshotOperation: operation aborted");
  state()->Finish();
  return status;
}

Status SnapshotOperation::DoReplicated(int64_t leader_term, Status* complete_status) {
  auto status = state()->Apply(leader_term);
  state()->Finish();
  return status;
}

string SnapshotOperation::ToString() const {
  return Substitute("SnapshotOperation [state=$0]", state()->ToString());
}

}  // namespace tablet
}  // namespace yb
