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

#ifndef YB_MASTER_CATALOG_ENTITY_INFO_H
#define YB_MASTER_CATALOG_ENTITY_INFO_H

#include <shared_mutex>

#include <mutex>

#include "yb/common/entity_ids.h"
#include "yb/common/index.h"
#include "yb/common/schema.h"
#include "yb/master/master.pb.h"
#include "yb/master/tasks_tracker.h"
#include "yb/master/ts_descriptor.h"
#include "yb/server/monitored_task.h"
#include "yb/util/cow_object.h"
#include "yb/util/monotime.h"

namespace yb {
namespace master {

// Information on a current replica of a tablet.
// This is copyable so that no locking is needed.
struct TabletReplica {
  TSDescriptor* ts_desc;
  tablet::RaftGroupStatePB state;
  consensus::RaftPeerPB::Role role;
  consensus::RaftPeerPB::MemberType member_type;
  MonoTime time_updated;

  // Replica is processing a parent data after a tablet splits, rocksdb sst files will have
  // metadata saying that either the first half, or the second half is irrelevant.
  bool processing_parent_data = false;

  TabletReplica() : time_updated(MonoTime::Now()) {}

  void UpdateFrom(const TabletReplica& source);

  bool IsStale() const;

  bool IsStarting() const;

  std::string ToString() const;
};

// This class is a base wrapper around the protos that get serialized in the data column of the
// sys_catalog. Subclasses of this will provide convenience getter/setter methods around the
// protos and instances of these will be wrapped around CowObjects and locks for access and
// modifications.
template <class DataEntryPB, SysRowEntry::Type entry_type>
struct Persistent {
  // Type declaration to be used in templated read/write methods. We are using typename
  // Class::data_type in templated methods for figuring out the type we need.
  typedef DataEntryPB data_type;

  // Subclasses of this need to provide a valid value of the entry type through
  // the template class argument.
  static SysRowEntry::Type type() { return entry_type; }

  // The proto that is persisted in the sys_catalog.
  DataEntryPB pb;
};

// This class is used to manage locking of the persistent metadata returned from the
// MetadataCowWrapper objects.
template <class MetadataClass>
class MetadataLock : public CowLock<typename MetadataClass::cow_state> {
 public:
  typedef CowLock<typename MetadataClass::cow_state> super;
  MetadataLock(MetadataClass* info, typename super::LockMode mode)
      : super(DCHECK_NOTNULL(info)->mutable_metadata(), mode) {}
  MetadataLock(const MetadataClass* info, typename super::LockMode mode)
      : super(&(DCHECK_NOTNULL(info))->metadata(), mode) {}
};

// This class is a base wrapper around accessors for the persistent proto data, through CowObject.
// The locks are taken on subclasses of this class, around the object returned from metadata().
template <class PersistentDataEntryPB>
class MetadataCowWrapper {
 public:
  // Type declaration for use in the Lock classes.
  typedef PersistentDataEntryPB cow_state;
  typedef MetadataLock<MetadataCowWrapper<PersistentDataEntryPB>> lock_type;

  // This method should return the id to be written into the sys_catalog id column.
  virtual const std::string& id() const = 0;

  // Pretty printing.
  virtual std::string ToString() const {
    return strings::Substitute(
        "Object type = $0 (id = $1)", PersistentDataEntryPB::type(), id());
  }

  // Access the persistent metadata. Typically you should use
  // MetadataLock to gain access to this data.
  const CowObject<PersistentDataEntryPB>& metadata() const { return metadata_; }
  CowObject<PersistentDataEntryPB>* mutable_metadata() { return &metadata_; }

  std::unique_ptr<lock_type> LockForRead() const {
    return std::unique_ptr<lock_type>(new lock_type(this, lock_type::READ));
  }

  std::unique_ptr<lock_type> LockForWrite() {
    return std::unique_ptr<lock_type>(new lock_type(this, lock_type::WRITE));
  }

 protected:
  virtual ~MetadataCowWrapper() = default;
  CowObject<PersistentDataEntryPB> metadata_;
};

// The data related to a tablet which is persisted on disk.
// This portion of TabletInfo is managed via CowObject.
// It wraps the underlying protobuf to add useful accessors.
struct PersistentTabletInfo : public Persistent<SysTabletsEntryPB, SysRowEntry::TABLET> {
  bool is_running() const {
    return pb.state() == SysTabletsEntryPB::RUNNING;
  }

  bool is_deleted() const {
    return pb.state() == SysTabletsEntryPB::REPLACED ||
           pb.state() == SysTabletsEntryPB::DELETED;
  }

  bool is_colocated() const {
    return pb.colocated();
  }

  // Helper to set the state of the tablet with a custom message.
  // Requires that the caller has prepared this object for write.
  // The change will only be visible after Commit().
  void set_state(SysTabletsEntryPB::State state, const std::string& msg);
};

class TableInfo;
typedef scoped_refptr<TableInfo> TableInfoPtr;

typedef std::unordered_map<TabletServerId, MonoTime> LeaderStepDownFailureTimes;

// The information about a single tablet which exists in the cluster,
// including its state and locations.
//
// This object uses copy-on-write for the portions of data which are persisted
// on disk. This allows the mutated data to be staged and written to disk
// while readers continue to access the previous version. These portions
// of data are in PersistentTabletInfo above, and typically accessed using
// MetadataLock. For example:
//
//   TabletInfo* tablet = ...;
//   MetadataLock l = tablet->LockForRead();
//   if (l.data().is_running()) { ... }
//
// The non-persistent information about the tablet is protected by an internal
// spin-lock.
//
// The object is owned/managed by the CatalogManager, and exposed for testing.
class TabletInfo : public RefCountedThreadSafe<TabletInfo>,
                   public MetadataCowWrapper<PersistentTabletInfo> {
 public:
  typedef std::unordered_map<std::string, TabletReplica> ReplicaMap;

  TabletInfo(const scoped_refptr<TableInfo>& table, TabletId tablet_id);
  virtual const TabletId& id() const override { return tablet_id_; }

  const TabletId& tablet_id() const { return tablet_id_; }
  scoped_refptr<const TableInfo> table() const { return table_; }
  const scoped_refptr<TableInfo>& table() { return table_; }

  // Accessors for the latest known tablet replica locations.
  // These locations include only the members of the latest-reported Raft
  // configuration whose tablet servers have ever heartbeated to this Master.
  void SetReplicaLocations(std::shared_ptr<ReplicaMap> replica_locations);
  std::shared_ptr<const ReplicaMap> GetReplicaLocations() const;
  Result<TSDescriptor*> GetLeader() const;

  // Replaces a replica in replica_locations_ map if it exists. Otherwise, it adds it to the map.
  void UpdateReplicaLocations(const TabletReplica& replica);

  // Accessors for the last time the replica locations were updated.
  void set_last_update_time(const MonoTime& ts);
  MonoTime last_update_time() const;

  // Accessors for the last reported schema version.
  bool set_reported_schema_version(const TableId& table_id, uint32_t version);
  uint32_t reported_schema_version(const TableId& table_id);

  bool colocated() const;

  // No synchronization needed.
  std::string ToString() const override;

  // This is called when a leader stepdown request fails. Optionally, takes an amount of time since
  // the stepdown failure, in case it happened in the past (e.g. we talked to a tablet server and
  // it told us that it previously tried to step down in favor of this server and that server lost
  // the election).
  void RegisterLeaderStepDownFailure(const TabletServerId& intended_leader,
                                     MonoDelta time_since_stepdown_failure);

  // Retrieves a map of recent leader step-down failures. At the same time, forgets about step-down
  // failures that happened before a certain point in time.
  void GetLeaderStepDownFailureTimes(MonoTime forget_failures_before,
                                     LeaderStepDownFailureTimes* dest);

  CHECKED_STATUS CheckRunning() const;

  bool InitiateElection() {
    bool expected = false;
    return initiated_election_.compare_exchange_strong(expected, true);
  }

 private:
  friend class RefCountedThreadSafe<TabletInfo>;

  class LeaderChangeReporter;
  friend class LeaderChangeReporter;

  ~TabletInfo();
  TSDescriptor* GetLeaderUnlocked() const;

  const TabletId tablet_id_;
  const scoped_refptr<TableInfo> table_;

  // Lock protecting the below mutable fields.
  // This doesn't protect metadata_ (the on-disk portion).
  mutable simple_spinlock lock_;

  // The last time the replica locations were updated.
  // Also set when the Master first attempts to create the tablet.
  MonoTime last_update_time_;

  // The locations in the latest Raft config where this tablet has been
  // reported. The map is keyed by tablet server UUID.
  std::shared_ptr<ReplicaMap> replica_locations_;

  // Reported schema version (in-memory only).
  std::unordered_map<TableId, uint32_t> reported_schema_version_ = {};

  LeaderStepDownFailureTimes leader_stepdown_failure_times_;

  std::atomic<bool> initiated_election_{false};

  DISALLOW_COPY_AND_ASSIGN(TabletInfo);
};

// The data related to a table which is persisted on disk.
// This portion of TableInfo is managed via CowObject.
// It wraps the underlying protobuf to add useful accessors.
struct PersistentTableInfo : public Persistent<SysTablesEntryPB, SysRowEntry::TABLE> {
  bool started_deleting() const {
    return pb.state() == SysTablesEntryPB::DELETING ||
           pb.state() == SysTablesEntryPB::DELETED;
  }

  bool is_deleted() const {
    return pb.state() == SysTablesEntryPB::DELETED;
  }

  bool is_deleting() const {
    return pb.state() == SysTablesEntryPB::DELETING;
  }

  bool is_running() const {
    return pb.state() == SysTablesEntryPB::RUNNING ||
           pb.state() == SysTablesEntryPB::ALTERING;
  }

  // Return the table's name.
  const TableName& name() const {
    return pb.name();
  }

  // Return the table's type.
  const TableType table_type() const {
    return pb.table_type();
  }

  // Return the table's namespace id.
  const NamespaceId& namespace_id() const { return pb.namespace_id(); }
  // Return the table's namespace name.
  const NamespaceName& namespace_name() const { return pb.namespace_name(); }

  const SchemaPB& schema() const {
    return pb.schema();
  }

  SchemaPB* mutable_schema() {
    return pb.mutable_schema();
  }

  // Helper to set the state of the tablet with a custom message.
  void set_state(SysTablesEntryPB::State state, const std::string& msg);
};

// The information about a table, including its state and tablets.
//
// This object uses copy-on-write techniques similarly to TabletInfo.
// Please see the TabletInfo class doc above for more information.
//
// The non-persistent information about the table is protected by an internal
// spin-lock.
class TableInfo : public RefCountedThreadSafe<TableInfo>,
                  public MetadataCowWrapper<PersistentTableInfo> {
 public:
  explicit TableInfo(TableId table_id, scoped_refptr<TasksTracker> tasks_tracker = nullptr);

  const TableName name() const;

  bool is_running() const;

  std::string ToString() const override;
  std::string ToStringWithState() const;

  const NamespaceId namespace_id() const;
  const NamespaceName namespace_name() const;

  const CHECKED_STATUS GetSchema(Schema* schema) const;

  bool colocated() const;

  // Return the table's ID. Does not require synchronization.
  virtual const std::string& id() const override { return table_id_; }

  // Return the indexed table id if the table is an index table. Otherwise, return an empty string.
  const std::string indexed_table_id() const;

  bool is_index() const {
    return !indexed_table_id().empty();
  }

  // For index table
  bool is_local_index() const;
  bool is_unique_index() const;

  void set_is_system() { is_system_ = true; }
  bool is_system() const { return is_system_; }

  // Return the table type of the table.
  TableType GetTableType() const;

  // Checks if the table is the internal redis table.
  bool IsRedisTable() const {
    return GetTableType() == REDIS_TABLE_TYPE;
  }

  // Add a tablet to this table.
  void AddTablet(TabletInfo *tablet);

  // Add multiple tablets to this table.
  void AddTablets(const std::vector<TabletInfo*>& tablets);

  // Return true if tablet with 'partition_key_start' has been
  // removed from 'tablet_map_' below.
  bool RemoveTablet(const std::string& partition_key_start);

  // This only returns tablets which are in RUNNING state.
  void GetTabletsInRange(const GetTableLocationsRequestPB* req, TabletInfos *ret) const;
  void GetTabletsInRange(
      const std::string& partition_key_start, const std::string& partition_key_end,
      TabletInfos* ret,
      int32_t max_returned_locations = std::numeric_limits<int32_t>::max()) const;

  std::size_t NumTablets() const;

  // Get all tablets of the table.
  void GetAllTablets(TabletInfos *ret) const;

  // Get the tablet of the table.  The table must be colocated.
  TabletInfoPtr GetColocatedTablet() const;

  // Get info of the specified index.
  IndexInfo GetIndexInfo(const TableId& index_id) const;

  // Returns true if all tablets of the table are deleted.
  bool AreAllTabletsDeleted() const;

  // Returns true if the table creation is in-progress.
  bool IsCreateInProgress() const;

  // Returns true if the table is backfilling an index.
  bool IsBackfilling() const {
    std::shared_lock<decltype(lock_)> l(lock_);
    return is_backfilling_;
  }

  void SetIsBackfilling(bool flag) {
    std::lock_guard<decltype(lock_)> l(lock_);
    is_backfilling_ = flag;
  }

  // Returns true if an "Alter" operation is in-progress.
  bool IsAlterInProgress(uint32_t version) const;

  // Set the Status related to errors on CreateTable.
  void SetCreateTableErrorStatus(const Status& status);

  // Get the Status of the last error from the current CreateTable.
  CHECKED_STATUS GetCreateTableErrorStatus() const;

  std::size_t NumLBTasks() const;
  std::size_t NumTasks() const;
  bool HasTasks() const;
  bool HasTasks(MonitoredTask::Type type) const;
  void AddTask(std::shared_ptr<MonitoredTask> task);
  void RemoveTask(const std::shared_ptr<MonitoredTask>& task);
  void AbortTasks();
  void AbortTasksAndClose();
  void WaitTasksCompletion();

  // Allow for showing outstanding tasks in the master UI.
  std::unordered_set<std::shared_ptr<MonitoredTask>> GetTasks();

 private:
  friend class RefCountedThreadSafe<TableInfo>;
  ~TableInfo();

  void AddTabletUnlocked(TabletInfo* tablet);
  void AbortTasksAndCloseIfRequested(bool close);

  const TableId table_id_;

  scoped_refptr<TasksTracker> tasks_tracker_;

  // Sorted index of tablet start partition-keys to TabletInfo.
  // The TabletInfo objects are owned by the CatalogManager.
  typedef std::map<std::string, TabletInfo *> TabletInfoMap;
  TabletInfoMap tablet_map_;

  // Protects tablet_map_ and pending_tasks_.
  mutable rw_spinlock lock_;

  // If closing, requests to AddTask will be promptly aborted.
  bool closing_ = false;

  // In memory state set during backfill to prevent multiple backfill jobs.
  bool is_backfilling_ = false;

  std::atomic<bool> is_system_{false};

  // List of pending tasks (e.g. create/alter tablet requests).
  std::unordered_set<std::shared_ptr<MonitoredTask>> pending_tasks_;

  // The last error Status of the currently running CreateTable. Will be OK, if freshly constructed
  // object, or if the CreateTable was successful.
  Status create_table_error_;

  DISALLOW_COPY_AND_ASSIGN(TableInfo);
};

class DeletedTableInfo;
typedef std::pair<TabletServerId, TabletId> TabletKey;
typedef std::unordered_map<
    TabletKey, scoped_refptr<DeletedTableInfo>, boost::hash<TabletKey>> DeletedTabletMap;

class DeletedTableInfo : public RefCountedThreadSafe<DeletedTableInfo> {
 public:
  explicit DeletedTableInfo(const TableInfo* table);

  const TableId& id() const { return table_id_; }

  std::size_t NumTablets() const;
  bool HasTablets() const;

  void DeleteTablet(const TabletKey& key);

  void AddTabletsToMap(DeletedTabletMap* tablet_map);

 private:
  const TableId table_id_;

  // Protects tablet_set_.
  mutable simple_spinlock lock_;

  typedef std::unordered_set<TabletKey, boost::hash<TabletKey>> TabletSet;
  TabletSet tablet_set_;
};

// The data related to a namespace which is persisted on disk.
// This portion of NamespaceInfo is managed via CowObject.
// It wraps the underlying protobuf to add useful accessors.
struct PersistentNamespaceInfo : public Persistent<SysNamespaceEntryPB, SysRowEntry::NAMESPACE> {
  // Get the namespace name.
  const NamespaceName& name() const {
    return pb.name();
  }

  YQLDatabase database_type() const {
    return pb.database_type();
  }

  bool colocated() const {
    return pb.colocated();
  }
};

// The information about a namespace.
//
// This object uses copy-on-write techniques similarly to TabletInfo.
// Please see the TabletInfo class doc above for more information.
class NamespaceInfo : public RefCountedThreadSafe<NamespaceInfo>,
                      public MetadataCowWrapper<PersistentNamespaceInfo> {
 public:
  explicit NamespaceInfo(NamespaceId ns_id);

  virtual const NamespaceId& id() const override { return namespace_id_; }

  const NamespaceName& name() const;

  YQLDatabase database_type() const;

  bool colocated() const;

  ::yb::master::SysNamespaceEntryPB_State state() const;

  std::string ToString() const override;

 private:
  friend class RefCountedThreadSafe<NamespaceInfo>;
  ~NamespaceInfo() = default;

  // The ID field is used in the sys_catalog table.
  const NamespaceId namespace_id_;

  DISALLOW_COPY_AND_ASSIGN(NamespaceInfo);
};

// The information about a tablegroup.
class TablegroupInfo : public RefCountedThreadSafe<TablegroupInfo>{
 public:
  explicit TablegroupInfo(TablegroupId tablegroup_id,
                          NamespaceId namespace_id);

  const std::string& id() const { return tablegroup_id_; }
  const std::string& namespace_id() const { return namespace_id_; }

  // Operations to track table_set_ information (what tables belong to the tablegroup)
  void AddChildTable(const TableId& table_id);
  void DeleteChildTable(const TableId& table_id);
  bool HasChildTables() const;
  std::size_t NumChildTables() const;

 private:
  friend class RefCountedThreadSafe<TablegroupInfo>;
  ~TablegroupInfo() = default;

  // The tablegroup ID is used in the catalog manager maps to look up the proper
  // tablet to add user tables to.
  const TablegroupId tablegroup_id_;
  const NamespaceId namespace_id_;

  // Protects table_set_.
  mutable simple_spinlock lock_;
  std::unordered_set<TableId> table_set_;

  DISALLOW_COPY_AND_ASSIGN(TablegroupInfo);
};

// The data related to a User-Defined Type which is persisted on disk.
// This portion of UDTypeInfo is managed via CowObject.
// It wraps the underlying protobuf to add useful accessors.
struct PersistentUDTypeInfo : public Persistent<SysUDTypeEntryPB, SysRowEntry::UDTYPE> {
  // Return the type's name.
  const UDTypeName& name() const {
    return pb.name();
  }

  // Return the table's namespace id.
  const NamespaceId& namespace_id() const {
    return pb.namespace_id();
  }

  int field_names_size() const {
    return pb.field_names_size();
  }

  const string& field_names(int index) const {
    return pb.field_names(index);
  }

  int field_types_size() const {
    return pb.field_types_size();
  }

  const QLTypePB& field_types(int index) const {
    return pb.field_types(index);
  }
};

class UDTypeInfo : public RefCountedThreadSafe<UDTypeInfo>,
                   public MetadataCowWrapper<PersistentUDTypeInfo> {
 public:
  explicit UDTypeInfo(UDTypeId udtype_id);

  // Return the user defined type's ID. Does not require synchronization.
  virtual const std::string& id() const override { return udtype_id_; }

  const UDTypeName& name() const;

  const NamespaceId& namespace_id() const;

  int field_names_size() const;

  const string& field_names(int index) const;

  int field_types_size() const;

  const QLTypePB& field_types(int index) const;

  std::string ToString() const override;

 private:
  friend class RefCountedThreadSafe<UDTypeInfo>;
  ~UDTypeInfo() = default;

  // The ID field is used in the sys_catalog table.
  const UDTypeId udtype_id_;

  DISALLOW_COPY_AND_ASSIGN(UDTypeInfo);
};

// This wraps around the proto containing cluster level config information. It will be used for
// CowObject managed access.
struct PersistentClusterConfigInfo : public Persistent<SysClusterConfigEntryPB,
                                                       SysRowEntry::CLUSTER_CONFIG> {
};

// This is the in memory representation of the cluster config information serialized proto data,
// using metadata() for CowObject access.
class ClusterConfigInfo : public RefCountedThreadSafe<ClusterConfigInfo>,
                          public MetadataCowWrapper<PersistentClusterConfigInfo> {
 public:
  ClusterConfigInfo() {}

  virtual const std::string& id() const override { return fake_id_; }

 private:
  friend class RefCountedThreadSafe<ClusterConfigInfo>;
  ~ClusterConfigInfo() = default;

  // We do not use the ID field in the sys_catalog table.
  const std::string fake_id_;

  DISALLOW_COPY_AND_ASSIGN(ClusterConfigInfo);
};

struct PersistentRedisConfigInfo
    : public Persistent<SysRedisConfigEntryPB, SysRowEntry::REDIS_CONFIG> {};

class RedisConfigInfo : public RefCountedThreadSafe<RedisConfigInfo>,
                        public MetadataCowWrapper<PersistentRedisConfigInfo> {
 public:
  explicit RedisConfigInfo(const std::string key) : config_key_(key) {}

  virtual const std::string& id() const override { return config_key_; }

 private:
  friend class RefCountedThreadSafe<RedisConfigInfo>;
  ~RedisConfigInfo() = default;

  const std::string config_key_;

  DISALLOW_COPY_AND_ASSIGN(RedisConfigInfo);
};

struct PersistentRoleInfo : public Persistent<SysRoleEntryPB, SysRowEntry::ROLE> {};

class RoleInfo : public RefCountedThreadSafe<RoleInfo>,
                 public MetadataCowWrapper<PersistentRoleInfo> {
 public:
  explicit RoleInfo(const std::string& role) : role_(role) {}
  const std::string& id() const override { return role_; }

 private:
  friend class RefCountedThreadSafe<RoleInfo>;
  ~RoleInfo() = default;

  const std::string role_;

  DISALLOW_COPY_AND_ASSIGN(RoleInfo);
};

struct PersistentSysConfigInfo
    : public Persistent<SysConfigEntryPB, SysRowEntry::SYS_CONFIG> {};

class SysConfigInfo : public RefCountedThreadSafe<SysConfigInfo>,
                      public MetadataCowWrapper<PersistentSysConfigInfo> {
 public:
  explicit SysConfigInfo(const std::string& config_type) : config_type_(config_type) {}
  const std::string& id() const override { return config_type_; /* config type is the entry id */ }

 private:
  friend class RefCountedThreadSafe<SysConfigInfo>;
  ~SysConfigInfo() = default;

  const std::string config_type_;

  DISALLOW_COPY_AND_ASSIGN(SysConfigInfo);
};

// Convenience typedefs.
// Table(t)InfoMap ordered for deterministic locking.
typedef std::map<TabletId, scoped_refptr<TabletInfo>> TabletInfoMap;
typedef std::map<TableId, scoped_refptr<TableInfo>> TableInfoMap;
typedef std::pair<NamespaceId, TableName> TableNameKey;
typedef std::unordered_map<
    TableNameKey, scoped_refptr<TableInfo>, boost::hash<TableNameKey>> TableInfoByNameMap;

typedef std::unordered_map<UDTypeId, scoped_refptr<UDTypeInfo>> UDTypeInfoMap;
typedef std::pair<NamespaceId, UDTypeName> UDTypeNameKey;
typedef std::unordered_map<
    UDTypeNameKey, scoped_refptr<UDTypeInfo>, boost::hash<UDTypeNameKey>> UDTypeInfoByNameMap;

template <class Info>
void FillInfoEntry(const Info& info, SysRowEntry* entry) {
  entry->set_id(info.id());
  entry->set_type(info.metadata().state().type());
  entry->set_data(info.metadata().state().pb.SerializeAsString());
}

}  // namespace master
}  // namespace yb

#endif  // YB_MASTER_CATALOG_ENTITY_INFO_H
