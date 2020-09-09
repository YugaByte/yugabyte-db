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

#ifndef YB_TABLET_TABLET_COMPONENT_H
#define YB_TABLET_TABLET_COMPONENT_H

#include "yb/tablet/tablet.h"

namespace yb {
namespace tablet {

// Base class for Tablet components, has access to private Tablet fields.
// For methods descriptions see comments for appropriate field or method in Tablet class.
class TabletComponent {
 public:
  explicit TabletComponent(Tablet* tablet) : tablet_(*tablet) {}

  Tablet& tablet() const {
    return tablet_;
  }

 protected:
  ScopedRWOperationPause PauseReadWriteOperations();

  CHECKED_STATUS ResetRocksDBs(Destroy destroy, DisableFlushOnShutdown disable_flush_on_shutdown);

  CHECKED_STATUS OpenRocksDBs();

  std::string LogPrefix() const;

  RaftGroupMetadata& metadata() const;

  RWOperationCounter& pending_op_counter() const;

  rocksdb::DB& regular_db() const;

  bool has_regular_db() const;

  rocksdb::DB& intents_db() const;

  bool has_intents_db() const;

  std::mutex& create_checkpoint_lock() const;

  rocksdb::Env& rocksdb_env() const;

 private:
  Tablet& tablet_;
};

} // namespace tablet
} // namespace yb

#endif // YB_TABLET_TABLET_COMPONENT_H
