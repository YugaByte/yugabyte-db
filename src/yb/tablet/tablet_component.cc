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

#include "yb/tablet/tablet_component.h"

namespace yb {
namespace tablet {

ScopedRWOperationPause TabletComponent::PauseReadWriteOperations() {
  return tablet_.PauseReadWriteOperations();
}

Status TabletComponent::ResetRocksDBs(
    Destroy destroy, DisableFlushOnShutdown disable_flush_on_shutdown) {
  return tablet_.ResetRocksDBs(destroy, disable_flush_on_shutdown);
}

Status TabletComponent::OpenRocksDBs() {
  return tablet_.OpenKeyValueTablet();
}

std::string TabletComponent::LogPrefix() const {
  return tablet_.LogPrefix();
}

RaftGroupMetadata& TabletComponent::metadata() const {
  return *tablet_.metadata();
}

RWOperationCounter& TabletComponent::pending_op_counter() const {
  return tablet_.pending_op_counter_;
}

rocksdb::DB& TabletComponent::regular_db() const {
  return *tablet_.regular_db_;
}

bool TabletComponent::has_regular_db() const {
  return tablet_.regular_db_ != nullptr;
}

rocksdb::DB& TabletComponent::intents_db() const {
  return *tablet_.intents_db_;
}

bool TabletComponent::has_intents_db() const {
  return tablet_.intents_db_ != nullptr;
}

std::mutex& TabletComponent::create_checkpoint_lock() const {
  return tablet_.create_checkpoint_lock_;
}

rocksdb::Env& TabletComponent::rocksdb_env() const {
  return tablet_.rocksdb_env();
}

} // namespace tablet
} // namespace yb
