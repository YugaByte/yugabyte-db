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

#include <memory>

#include "yb/util/logging.h"
#include "yb/util/mutex.h"

#include "yb/master/catalog_manager_bg_tasks.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/scoped_leader_shared_lock.h"
#include "yb/master/ts_descriptor.h"
#include "yb/master/cluster_balance.h"
#include "yb/master/encryption_manager.h"
#include "yb/util/flag_tags.h"

using std::shared_ptr;

DEFINE_int32(catalog_manager_bg_task_wait_ms, 1000,
             "Amount of time the catalog manager background task thread waits "
             "between runs");
TAG_FLAG(catalog_manager_bg_task_wait_ms, hidden);

DEFINE_int32(load_balancer_initial_delay_secs, 120,
             "Amount of time to wait between becoming master leader and enabling the load "
             "balancer.");

namespace yb {
namespace master {

void CatalogManagerBgTasks::Wake() {
  MutexLock lock(lock_);
  pending_updates_ = true;
  cond_.Broadcast();
}

void CatalogManagerBgTasks::Wait(int msec) {
  MutexLock lock(lock_);
  if (closing_.load()) return;
  if (!pending_updates_) {
    cond_.TimedWait(MonoDelta::FromMilliseconds(msec));
  }
  pending_updates_ = false;
}

void CatalogManagerBgTasks::WakeIfHasPendingUpdates() {
  MutexLock lock(lock_);
  if (pending_updates_) {
    cond_.Broadcast();
  }
}

Status CatalogManagerBgTasks::Init() {
  RETURN_NOT_OK(yb::Thread::Create("catalog manager", "bgtasks",
      &CatalogManagerBgTasks::Run, this, &thread_));
  return Status::OK();
}

void CatalogManagerBgTasks::Shutdown() {
  {
    bool closing_expected = false;
    if (!closing_.compare_exchange_strong(closing_expected, true)) {
      VLOG(2) << "CatalogManagerBgTasks already shut down";
      return;
    }
  }

  Wake();
  if (thread_ != nullptr) {
    CHECK_OK(ThreadJoiner(thread_.get()).Join());
  }
}

void CatalogManagerBgTasks::Run() {
  while (!closing_.load()) {
    // Perform assignment processing.
    ScopedLeaderSharedLock l(catalog_manager_);
    if (!l.catalog_status().ok()) {
      LOG(WARNING) << "Catalog manager background task thread going to sleep: "
                   << l.catalog_status().ToString();
    } else if (l.leader_status().ok()) {
      // Clear metrics for dead tservers.
      vector<shared_ptr<TSDescriptor>> descs;
      const auto& ts_manager = catalog_manager_->master_->ts_manager();
      ts_manager->GetAllDescriptors(&descs);
      for (auto& ts_desc : descs) {
        if (!ts_manager->IsTSLive(ts_desc)) {
          ts_desc->ClearMetrics();
        }
      }

      // Report metrics.
      catalog_manager_->ReportMetrics();

      // Cleanup old tasks from tracker.
      catalog_manager_->tasks_tracker_->CleanupOldTasks();

      TabletInfos to_delete;
      TabletInfos to_process;

      // Get list of tablets not yet running or already replaced.
      catalog_manager_->ExtractTabletsToProcess(&to_delete, &to_process);

      if (!to_process.empty()) {
        // Transition tablet assignment state from preparing to creating, send
        // and schedule creation / deletion RPC messages, etc.
        Status s = catalog_manager_->ProcessPendingAssignments(to_process);
        if (!s.ok()) {
          // If there is an error (e.g., we are not the leader) abort this task
          // and wait until we're woken up again.
          //
          // TODO Add tests for this in the revision that makes
          // create/alter fault tolerant.
          LOG(ERROR) << "Error processing pending assignments, aborting the current task: "
                     << s.ToString();
        }
      } else {
        if (catalog_manager_->TimeSinceElectedLeader() >
            MonoDelta::FromSeconds(FLAGS_load_balancer_initial_delay_secs)) {
          catalog_manager_->load_balance_policy_->RunLoadBalancer();
        }
      }

      if (!to_delete.empty() || catalog_manager_->AreTablesDeleting()) {
        catalog_manager_->CleanUpDeletedTables();
      }
      std::vector<scoped_refptr<CDCStreamInfo>> streams;
      auto s = catalog_manager_->FindCDCStreamsMarkedAsDeleting(&streams);
      if (s.ok() && !streams.empty()) {
        s = catalog_manager_->CleanUpDeletedCDCStreams(streams);
      }
    }
    WARN_NOT_OK(catalog_manager_->encryption_manager_->
                GetUniverseKeyRegistry(&catalog_manager_->master_->proxy_cache()),
                "Could not schedule GetUniverseKeyRegistry task.");
    // Wait for a notification or a timeout expiration.
    //  - CreateTable will call Wake() to notify about the tablets to add
    //  - HandleReportedTablet/ProcessPendingAssignments will call WakeIfHasPendingUpdates()
    //    to notify about tablets creation.
    //  - DeleteTable will call Wake() to finish destructing any table internals
    l.Unlock();
    Wait(FLAGS_catalog_manager_bg_task_wait_ms);
  }
  VLOG(1) << "Catalog manager background task thread shutting down";
}

}  // namespace master
}  // namespace yb
