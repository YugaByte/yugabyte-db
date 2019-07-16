// Copyright (c) YugaByte, Inc.

#ifndef ENT_SRC_YB_TABLET_TABLET_BOOTSTRAP_H
#define ENT_SRC_YB_TABLET_TABLET_BOOTSTRAP_H

#include "../../../../src/yb/tablet/tablet_bootstrap.h"

namespace yb {
namespace tablet {
namespace enterprise {

class TabletBootstrap : public yb::tablet::TabletBootstrap {
  typedef yb::tablet::TabletBootstrap super;
 public:
  explicit TabletBootstrap(const BootstrapTabletData& data) : super(data) {}

 protected:
  Status PlayTabletSnapshotOpRequest(consensus::ReplicateMsg* replicate_msg);

  CHECKED_STATUS HandleOperation(consensus::OperationType op_type,
      consensus::ReplicateMsg* replicate, const consensus::CommitMsg* commit) override;

 private:
  DISALLOW_COPY_AND_ASSIGN(TabletBootstrap);
};

}  // namespace enterprise
}  // namespace tablet
}  // namespace yb

#endif // ENT_SRC_YB_TABLET_TABLET_BOOTSTRAP_H
