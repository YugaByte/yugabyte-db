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

#include <string>
#include <mutex>

#include "yb/master/catalog_entity_info.h"
#include "yb/util/format.h"
#include "yb/util/locks.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/common/wire_protocol.h"

using std::string;

using strings::Substitute;

DECLARE_int32(tserver_unresponsive_timeout_ms);

namespace yb {
namespace master {

// ================================================================================================
// TabletReplica
// ================================================================================================

string TabletReplica::ToString() const {
  return Format("{ ts_desc: $0 state: $1 role: $2 member_type: $3 "
                "processing_parent_data: $4 time since update: $5ms }",
                ts_desc->permanent_uuid(),
                tablet::RaftGroupStatePB_Name(state),
                consensus::RaftPeerPB_Role_Name(role),
                consensus::RaftPeerPB::MemberType_Name(member_type),
                processing_parent_data,
                MonoTime::Now().GetDeltaSince(time_updated).ToMilliseconds());
}

void TabletReplica::UpdateFrom(const TabletReplica& source) {
  state = source.state;
  role = source.role;
  member_type = source.member_type;
  processing_parent_data = source.processing_parent_data;
  time_updated = MonoTime::Now();
}

bool TabletReplica::IsStale() const {
  MonoTime now(MonoTime::Now());
  if (now.GetDeltaSince(time_updated).ToMilliseconds() >=
      GetAtomicFlag(&FLAGS_tserver_unresponsive_timeout_ms)) {
    return true;
  }
  return false;
}

bool TabletReplica::IsStarting() const {
  return (state == tablet::NOT_STARTED || state == tablet::BOOTSTRAPPING);
}

// ================================================================================================
// TabletInfo
// ================================================================================================

class TabletInfo::LeaderChangeReporter {
 public:
  explicit LeaderChangeReporter(TabletInfo* info)
      : info_(info), old_leader_(info->GetLeaderUnlocked()) {
  }

  ~LeaderChangeReporter() {
    auto new_leader = info_->GetLeaderUnlocked();
    if (old_leader_ != new_leader) {
      LOG(INFO) << "T " << info_->tablet_id() << ": Leader changed from "
                << yb::ToString(old_leader_) << " to " << yb::ToString(new_leader);
    }
  }
 private:
  TabletInfo* info_;
  TSDescriptor* old_leader_;
};


TabletInfo::TabletInfo(const scoped_refptr<TableInfo>& table, TabletId tablet_id)
    : tablet_id_(std::move(tablet_id)),
      table_(table),
      last_update_time_(MonoTime::Now()),
      reported_schema_version_({}) {
  // Have to pre-initialize to an empty map, in case of access before the first setter is called.
  replica_locations_ = std::make_shared<TabletInfo::ReplicaMap>();
}

TabletInfo::~TabletInfo() {
}

void TabletInfo::SetReplicaLocations(
    std::shared_ptr<TabletInfo::ReplicaMap> replica_locations) {
  std::lock_guard<simple_spinlock> l(lock_);
  LeaderChangeReporter leader_change_reporter(this);
  last_update_time_ = MonoTime::Now();
  replica_locations_ = replica_locations;
}

CHECKED_STATUS TabletInfo::CheckRunning() const {
  if (!table()->is_running()) {
    return STATUS_FORMAT(Expired, "Table is not running: $0", table()->ToStringWithState());
  }

  return Status::OK();
}

Result<TSDescriptor*> TabletInfo::GetLeader() const {
  std::lock_guard<simple_spinlock> l(lock_);
  auto result = GetLeaderUnlocked();
  if (result) {
    return result;
  }

  RETURN_NOT_OK(CheckRunning());

  return STATUS_FORMAT(
      NotFound,
      "No leader found for tablet $0 with $1 replicas: $2.",
      ToString(), replica_locations_->size(), *replica_locations_);
}

TSDescriptor* TabletInfo::GetLeaderUnlocked() const {
  for (const auto& pair : *replica_locations_) {
    if (pair.second.role == consensus::RaftPeerPB::LEADER) {
      return pair.second.ts_desc;
    }
  }
  return nullptr;
}

std::shared_ptr<const TabletInfo::ReplicaMap> TabletInfo::GetReplicaLocations() const {
  std::lock_guard<simple_spinlock> l(lock_);
  return replica_locations_;
}

void TabletInfo::UpdateReplicaLocations(const TabletReplica& replica) {
  std::lock_guard<simple_spinlock> l(lock_);
  LeaderChangeReporter leader_change_reporter(this);
  last_update_time_ = MonoTime::Now();
  // Make a new shared_ptr, copying the data, to ensure we don't race against access to data from
  // clients that already have the old shared_ptr.
  replica_locations_ = std::make_shared<TabletInfo::ReplicaMap>(*replica_locations_);
  auto it = replica_locations_->find(replica.ts_desc->permanent_uuid());
  if (it == replica_locations_->end()) {
    replica_locations_->emplace(replica.ts_desc->permanent_uuid(), replica);
    return;
  }
  it->second.UpdateFrom(replica);
}

void TabletInfo::set_last_update_time(const MonoTime& ts) {
  std::lock_guard<simple_spinlock> l(lock_);
  last_update_time_ = ts;
}

MonoTime TabletInfo::last_update_time() const {
  std::lock_guard<simple_spinlock> l(lock_);
  return last_update_time_;
}

bool TabletInfo::set_reported_schema_version(const TableId& table_id, uint32_t version) {
  std::lock_guard<simple_spinlock> l(lock_);
  if (reported_schema_version_.count(table_id) == 0 ||
      version > reported_schema_version_[table_id]) {
    reported_schema_version_[table_id] = version;
    return true;
  }
  return false;
}

uint32_t TabletInfo::reported_schema_version(const TableId& table_id) {
  std::lock_guard<simple_spinlock> l(lock_);
  if (reported_schema_version_.count(table_id) == 0) {
    return 0;
  }
  return reported_schema_version_[table_id];
}

bool TabletInfo::colocated() const {
  auto l = LockForRead();
  return l->data().pb.colocated();
}

string TabletInfo::ToString() const {
  return Substitute("$0 (table $1)", tablet_id_,
                    (table_ != nullptr ? table_->ToString() : "MISSING"));
}

void TabletInfo::RegisterLeaderStepDownFailure(const TabletServerId& dest_leader,
                                               MonoDelta time_since_stepdown_failure) {
  std::lock_guard<simple_spinlock> l(lock_);
  leader_stepdown_failure_times_[dest_leader] = MonoTime::Now() - time_since_stepdown_failure;
}

void TabletInfo::GetLeaderStepDownFailureTimes(MonoTime forget_failures_before,
                                               LeaderStepDownFailureTimes* dest) {
  std::lock_guard<simple_spinlock> l(lock_);
  for (auto iter = leader_stepdown_failure_times_.begin();
       iter != leader_stepdown_failure_times_.end(); ) {
    if (iter->second < forget_failures_before) {
      iter = leader_stepdown_failure_times_.erase(iter);
    } else {
      iter++;
    }
  }
  *dest = leader_stepdown_failure_times_;
}

void PersistentTabletInfo::set_state(SysTabletsEntryPB::State state, const string& msg) {
  pb.set_state(state);
  pb.set_state_msg(msg);
}

// ================================================================================================
// TableInfo
// ================================================================================================

TableInfo::TableInfo(TableId table_id, scoped_refptr<TasksTracker> tasks_tracker)
    : table_id_(std::move(table_id)),
      tasks_tracker_(tasks_tracker) {
}

TableInfo::~TableInfo() {
}

const TableName TableInfo::name() const {
  auto l = LockForRead();
  return l->data().pb.name();
}

bool TableInfo::is_running() const {
  auto l = LockForRead();
  return l->data().is_running();
}

string TableInfo::ToString() const {
  auto l = LockForRead();
  return Substitute("$0 [id=$1]", l->data().pb.name(), table_id_);
}

string TableInfo::ToStringWithState() const {
  auto l = LockForRead();
  return Substitute("$0 [id=$1, state=$2]",
      l->data().pb.name(), table_id_, SysTablesEntryPB::State_Name(l->data().pb.state()));
}

const NamespaceId TableInfo::namespace_id() const {
  auto l = LockForRead();
  return l->data().namespace_id();
}

const NamespaceName TableInfo::namespace_name() const {
  auto l = LockForRead();
  return l->data().namespace_name();
}

const Status TableInfo::GetSchema(Schema* schema) const {
  auto l = LockForRead();
  return SchemaFromPB(l->data().schema(), schema);
}

bool TableInfo::colocated() const {
  auto l = LockForRead();
  return l->data().pb.colocated();
}

const string TableInfo::indexed_table_id() const {
  auto l = LockForRead();
  return l->data().pb.has_index_info()
             ? l->data().pb.index_info().indexed_table_id()
             : l->data().pb.has_indexed_table_id() ? l->data().pb.indexed_table_id() : "";
}

bool TableInfo::is_local_index() const {
  auto l = LockForRead();
  return l->data().pb.has_index_info() ? l->data().pb.index_info().is_local()
                                       : l->data().pb.is_local_index();
}

bool TableInfo::is_unique_index() const {
  auto l = LockForRead();
  return l->data().pb.has_index_info() ? l->data().pb.index_info().is_unique()
                                       : l->data().pb.is_unique_index();
}

TableType TableInfo::GetTableType() const {
  auto l = LockForRead();
  return l->data().pb.table_type();
}

bool TableInfo::RemoveTablet(const string& partition_key_start) {
  std::lock_guard<decltype(lock_)> l(lock_);
  return EraseKeyReturnValuePtr(&tablet_map_, partition_key_start) != NULL;
}

void TableInfo::AddTablet(TabletInfo *tablet) {
  std::lock_guard<decltype(lock_)> l(lock_);
  AddTabletUnlocked(tablet);
}

void TableInfo::AddTablets(const vector<TabletInfo*>& tablets) {
  std::lock_guard<decltype(lock_)> l(lock_);
  for (TabletInfo *tablet : tablets) {
    AddTabletUnlocked(tablet);
  }
}

void TableInfo::AddTabletUnlocked(TabletInfo* tablet) {
  const auto& tablet_meta = tablet->metadata().dirty().pb;
  const auto& partition_key_start = tablet_meta.partition().partition_key_start();
  auto it = tablet_map_.find(partition_key_start);
  if (it == tablet_map_.end()) {
    tablet_map_.emplace(partition_key_start, tablet);
  } else {
    const auto old_split_depth = it->second->LockForRead()->data().pb.split_depth();
    if (tablet_meta.split_depth() < old_split_depth) {
      return;
    }
    VLOG(1) << "Replacing tablet " << it->second->tablet_id()
            << " (split_depth = " << old_split_depth << ")"
            << " with tablet " << tablet->tablet_id()
            << " (split_depth = " << tablet_meta.split_depth() << ")";
    it->second = tablet;
    // TODO: can we assert that the replaced tablet is not in Running state?
    // May be a little tricky since we don't know whether to look at its committed or
    // uncommitted state.
  }
}

void TableInfo::GetTabletsInRange(const GetTableLocationsRequestPB* req, TabletInfos* ret) const {
  GetTabletsInRange(
      req->partition_key_start(), req->partition_key_end(), ret, req->max_returned_locations());
}

void TableInfo::GetTabletsInRange(
    const std::string& partition_key_start, const std::string& partition_key_end,
    TabletInfos* ret, const int32_t max_returned_locations) const {
  shared_lock<decltype(lock_)> l(lock_);

  TableInfo::TabletInfoMap::const_iterator it, it_end;
  if (partition_key_start.empty()) {
    it = tablet_map_.begin();
  } else {
    it = tablet_map_.upper_bound(partition_key_start);
    if (it != tablet_map_.begin()) {
      --it;
    }
  }
  if (partition_key_end.empty()) {
    it_end = tablet_map_.end();
  } else {
    it_end = tablet_map_.upper_bound(partition_key_end);
  }

  int32_t count = 0;
  for (; it != it_end && count < max_returned_locations; ++it) {
    ret->push_back(make_scoped_refptr(it->second));
    count++;
  }
}

bool TableInfo::IsAlterInProgress(uint32_t version) const {
  shared_lock<decltype(lock_)> l(lock_);
  for (const TableInfo::TabletInfoMap::value_type& e : tablet_map_) {
    if (e.second->reported_schema_version(table_id_) < version) {
      VLOG(3) << "Table " << table_id_ << " ALTER in progress due to tablet "
              << e.second->ToString() << " because reported schema "
              << e.second->reported_schema_version(table_id_) << " < expected " << version;
      return true;
    }
  }
  return false;
}
bool TableInfo::AreAllTabletsDeleted() const {
  shared_lock<decltype(lock_)> l(lock_);
  for (const TableInfo::TabletInfoMap::value_type& e : tablet_map_) {
    auto tablet_lock = e.second->LockForRead();
    if (!tablet_lock->data().is_deleted()) {
      return false;
    }
  }
  return true;
}

bool TableInfo::IsCreateInProgress() const {
  shared_lock<decltype(lock_)> l(lock_);
  for (const TableInfo::TabletInfoMap::value_type& e : tablet_map_) {
    auto tablet_lock = e.second->LockForRead();
    if (!tablet_lock->data().is_running()) {
      return true;
    }
  }
  return false;
}

void TableInfo::SetCreateTableErrorStatus(const Status& status) {
  std::lock_guard<decltype(lock_)> l(lock_);
  create_table_error_ = status;
}

Status TableInfo::GetCreateTableErrorStatus() const {
  shared_lock<decltype(lock_)> l(lock_);
  return create_table_error_;
}

std::size_t TableInfo::NumLBTasks() const {
  shared_lock<decltype(lock_)> l(lock_);
  return std::count_if(pending_tasks_.begin(),
                       pending_tasks_.end(),
                       [](auto task) { return task->started_by_lb(); });
}

std::size_t TableInfo::NumTasks() const {
  shared_lock<decltype(lock_)> l(lock_);
  return pending_tasks_.size();
}

bool TableInfo::HasTasks() const {
  shared_lock<decltype(lock_)> l(lock_);
  return !pending_tasks_.empty();
}

bool TableInfo::HasTasks(MonitoredTask::Type type) const {
  shared_lock<decltype(lock_)> l(lock_);
  for (auto task : pending_tasks_) {
    if (task->type() == type) {
      return true;
    }
  }
  return false;
}

void TableInfo::AddTask(std::shared_ptr<MonitoredTask> task) {
  bool abort_task = false;
  {
    std::lock_guard<decltype(lock_)> l(lock_);
    if (!closing_) {
      pending_tasks_.insert(task);
      if (tasks_tracker_) {
        tasks_tracker_->AddTask(task);
      }
    } else {
      abort_task = true;
    }
  }
  // We need to abort these tasks without holding the lock because when a task is destroyed it tries
  // to acquire the same lock to remove itself from pending_tasks_.
  if (abort_task) {
    task->AbortAndReturnPrevState(STATUS(Aborted, "Table closing"));
  }
}

void TableInfo::RemoveTask(const std::shared_ptr<MonitoredTask>& task) {
  {
    std::lock_guard<decltype(lock_)> l(lock_);
    pending_tasks_.erase(task);
  }
  VLOG(1) << __func__ << " Removed task " << task.get() << " " << task->description();
}

// Aborts tasks which have their rpc in progress, rest of them are aborted and also erased
// from the pending list.
void TableInfo::AbortTasks() {
  AbortTasksAndCloseIfRequested( /* close */ false);
}

void TableInfo::AbortTasksAndClose() {
  AbortTasksAndCloseIfRequested( /* close */ true);
}

void TableInfo::AbortTasksAndCloseIfRequested(bool close) {
  std::vector<std::shared_ptr<MonitoredTask>> abort_tasks;
  {
    std::lock_guard<decltype(lock_)> l(lock_);
    if (close) {
      closing_ = true;
    }
    abort_tasks.reserve(pending_tasks_.size());
    abort_tasks.assign(pending_tasks_.cbegin(), pending_tasks_.cend());
  }
  // We need to abort these tasks without holding the lock because when a task is destroyed it tries
  // to acquire the same lock to remove itself from pending_tasks_.
  for (const auto& task : abort_tasks) {
    VLOG(1) << __func__ << " Aborting task " << task.get() << " " << task->description();
    task->AbortAndReturnPrevState(STATUS(Aborted, "Table closing"));
  }
}

void TableInfo::WaitTasksCompletion() {
  int wait_time = 5;
  while (1) {
    std::vector<std::shared_ptr<MonitoredTask>> waiting_on_for_debug;
    {
      shared_lock<decltype(lock_)> l(lock_);
      if (pending_tasks_.empty()) {
        break;
      } else if (VLOG_IS_ON(1)) {
        waiting_on_for_debug.reserve(pending_tasks_.size());
        waiting_on_for_debug.assign(pending_tasks_.cbegin(), pending_tasks_.cend());
      }
    }
    for (const auto& task : waiting_on_for_debug) {
      VLOG(1) << "Waiting for Aborting task " << task.get() << " " << task->description();
    }
    base::SleepForMilliseconds(wait_time);
    wait_time = std::min(wait_time * 5 / 4, 10000);
  }
}

std::unordered_set<std::shared_ptr<MonitoredTask>> TableInfo::GetTasks() {
  shared_lock<decltype(lock_)> l(lock_);
  return pending_tasks_;
}

std::size_t TableInfo::NumTablets() const {
  shared_lock<decltype(lock_)> l(lock_);
  return tablet_map_.size();
}

void TableInfo::GetAllTablets(TabletInfos *ret) const {
  ret->clear();
  shared_lock<decltype(lock_)> l(lock_);
  for (const TableInfo::TabletInfoMap::value_type& e : tablet_map_) {
    ret->push_back(make_scoped_refptr(e.second));
  }
}

TabletInfoPtr TableInfo::GetColocatedTablet() const {
  shared_lock<decltype(lock_)> l(lock_);
  if (colocated()) {
    for (const TableInfo::TabletInfoMap::value_type& e : tablet_map_) {
      return make_scoped_refptr(e.second);
    }
  }
  return nullptr;
}

IndexInfo TableInfo::GetIndexInfo(const TableId& index_id) const {
  auto l = LockForRead();
  for (const auto& index_info_pb : l->data().pb.indexes()) {
    if (index_info_pb.table_id() == index_id) {
      return IndexInfo(index_info_pb);
    }
  }
  return IndexInfo();
}

void PersistentTableInfo::set_state(SysTablesEntryPB::State state, const string& msg) {
  VLOG(2) << __PRETTY_FUNCTION__ << " setting state for " << name() << " to "
          << SysTablesEntryPB::State_Name(state) << " reason: " << msg;
  pb.set_state(state);
  pb.set_state_msg(msg);
}

// ================================================================================================
// DeletedTableInfo
// ================================================================================================

DeletedTableInfo::DeletedTableInfo(const TableInfo* table) : table_id_(table->id()) {
  vector<scoped_refptr<TabletInfo>> tablets;
  table->GetAllTablets(&tablets);

  for (const scoped_refptr<TabletInfo>& tablet : tablets) {
    auto tablet_lock = tablet->LockForRead();
    auto replica_locations = tablet->GetReplicaLocations();

    for (const TabletInfo::ReplicaMap::value_type& r : *replica_locations) {
      tablet_set_.insert(TabletSet::value_type(
          r.second.ts_desc->permanent_uuid(), tablet->id()));
    }
  }
}

std::size_t DeletedTableInfo::NumTablets() const {
  std::lock_guard<simple_spinlock> l(lock_);
  return tablet_set_.size();
}

bool DeletedTableInfo::HasTablets() const {
  std::lock_guard<simple_spinlock> l(lock_);
  return !tablet_set_.empty();
}

void DeletedTableInfo::DeleteTablet(const TabletKey& key) {
  std::lock_guard<simple_spinlock> l(lock_);
  tablet_set_.erase(key);
}

void DeletedTableInfo::AddTabletsToMap(DeletedTabletMap* tablet_map) {
  std::lock_guard<simple_spinlock> l(lock_);
  for (const TabletKey& key : tablet_set_) {
    tablet_map->insert(DeletedTabletMap::value_type(key, this));
  }
}

// ================================================================================================
// NamespaceInfo
// ================================================================================================

NamespaceInfo::NamespaceInfo(NamespaceId ns_id) : namespace_id_(std::move(ns_id)) {}

const NamespaceName& NamespaceInfo::name() const {
  auto l = LockForRead();
  return l->data().pb.name();
}

YQLDatabase NamespaceInfo::database_type() const {
  auto l = LockForRead();
  return l->data().pb.database_type();
}

bool NamespaceInfo::colocated() const {
  auto l = LockForRead();
  return l->data().pb.colocated();
}

::yb::master::SysNamespaceEntryPB_State NamespaceInfo::state() const {
  auto l = LockForRead();
  return l->data().pb.state();
}

string NamespaceInfo::ToString() const {
  return Substitute("$0 [id=$1]", name(), namespace_id_);
}

// ================================================================================================
// TablegroupInfo
// ================================================================================================

TablegroupInfo::TablegroupInfo(TablegroupId tablegroup_id, NamespaceId namespace_id) :
                               tablegroup_id_(tablegroup_id), namespace_id_(namespace_id) {}

void TablegroupInfo::AddChildTable(const TableId& table_id) {
  std::lock_guard<simple_spinlock> l(lock_);
  if (table_set_.find(table_id) != table_set_.end()) {
    LOG(WARNING) << "Table ID " << table_id << " already in Tablegroup " << tablegroup_id_;
  } else {
    table_set_.insert(table_id);
  }
}

void TablegroupInfo::DeleteChildTable(const TableId& table_id) {
  std::lock_guard<simple_spinlock> l(lock_);
  if (table_set_.find(table_id) != table_set_.end()) {
    table_set_.erase(table_id);
  } else {
    LOG(WARNING) << "Table ID " << table_id << " not found in Tablegroup " << tablegroup_id_;
  }
}

bool TablegroupInfo::HasChildTables() const {
  std::lock_guard<simple_spinlock> l(lock_);
  return !table_set_.empty();
}


std::size_t TablegroupInfo::NumChildTables() const {
  std::lock_guard<simple_spinlock> l(lock_);
  return table_set_.size();
}

// ================================================================================================
// UDTypeInfo
// ================================================================================================

UDTypeInfo::UDTypeInfo(UDTypeId udtype_id) : udtype_id_(std::move(udtype_id)) { }

const UDTypeName& UDTypeInfo::name() const {
  auto l = LockForRead();
  return l->data().pb.name();
}

const NamespaceName& UDTypeInfo::namespace_id() const {
  auto l = LockForRead();
  return l->data().pb.namespace_id();
}

int UDTypeInfo::field_names_size() const {
  auto l = LockForRead();
  return l->data().pb.field_names_size();
}

const string& UDTypeInfo::field_names(int index) const {
  auto l = LockForRead();
  return l->data().pb.field_names(index);
}

int UDTypeInfo::field_types_size() const {
  auto l = LockForRead();
  return l->data().pb.field_types_size();
}

const QLTypePB& UDTypeInfo::field_types(int index) const {
  auto l = LockForRead();
  return l->data().pb.field_types(index);
}

string UDTypeInfo::ToString() const {
  auto l = LockForRead();
  return Substitute("$0 [id=$1] {metadata=$2} ", name(), udtype_id_, l->data().pb.DebugString());
}

}  // namespace master
}  // namespace yb
