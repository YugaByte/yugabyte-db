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

#ifndef YB_TABLET_OPERATIONS_SNAPSHOT_OPERATION_H
#define YB_TABLET_OPERATIONS_SNAPSHOT_OPERATION_H

#include <mutex>
#include <string>

#include "yb/tablet/tablet_fwd.h"
#include "yb/gutil/macros.h"
#include "yb/tablet/operations/operation.h"
#include "yb/util/locks.h"

namespace yb {
namespace tablet {

// Operation Context for the TabletSnapshot operation.
// Keeps track of the Operation states (request, result, ...)
class SnapshotOperationState :
    public ExclusiveSchemaOperationState<tserver::TabletSnapshotOpRequestPB> {
 public:
  ~SnapshotOperationState() = default;

  SnapshotOperationState(Tablet* tablet,
                         const tserver::TabletSnapshotOpRequestPB* request = nullptr)
      : ExclusiveSchemaOperationState(tablet, request) {
  }

  tserver::TabletSnapshotOpRequestPB::Operation operation() const {
    return request() == nullptr ?
        tserver::TabletSnapshotOpRequestPB::UNKNOWN : request()->operation();
  }

  void UpdateRequestFromConsensusRound() override;

  CHECKED_STATUS Apply(int64_t leader_term);

  std::string ToString() const override;

  // Returns the snapshot directory, based on the tablet's top directory for all snapshots, and any
  // overrides for the snapshot directory this operation might have.
  std::string GetSnapshotDir(const std::string& top_snapshots_dir) const;

  bool CheckOperationRequirements();

 private:
  DISALLOW_COPY_AND_ASSIGN(SnapshotOperationState);
};

// Executes the TabletSnapshotOp operation.
class SnapshotOperation : public Operation {
 public:
  explicit SnapshotOperation(std::unique_ptr<SnapshotOperationState> tx_state);

  SnapshotOperationState* state() override {
    return down_cast<SnapshotOperationState*>(Operation::state());
  }

  const SnapshotOperationState* state() const override {
    return down_cast<const SnapshotOperationState*>(Operation::state());
  }

  consensus::ReplicateMsgPtr NewReplicateMsg() override;

  CHECKED_STATUS Prepare() override;

  std::string ToString() const override;

 private:
  // Starts the TabletSnapshotOp operation by assigning it a timestamp.
  void DoStart() override;
  CHECKED_STATUS DoReplicated(int64_t leader_term, Status* complete_status) override;
  CHECKED_STATUS DoAborted(const Status& status) override;

  DISALLOW_COPY_AND_ASSIGN(SnapshotOperation);
};

}  // namespace tablet
}  // namespace yb

#endif  // YB_TABLET_OPERATIONS_SNAPSHOT_OPERATION_H
