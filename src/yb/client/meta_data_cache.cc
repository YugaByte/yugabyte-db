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

#include "yb/client/meta_data_cache.h"

#include "yb/client/client.h"
#include "yb/client/table.h"
#include "yb/client/yb_table_name.h"

DEFINE_int32(update_permissions_cache_msecs, 2000,
             "How often the roles' permissions cache should be updated. 0 means never update it");

namespace yb {
namespace client {

namespace {

Status GenerateUnauthorizedError(const std::string& canonical_resource,
                                 const ql::ObjectType& object_type,
                                 const RoleName& role_name,
                                 const PermissionType& permission,
                                 const NamespaceName& keyspace,
                                 const TableName& table) {
  switch (object_type) {
    case ql::ObjectType::OBJECT_TABLE:
      return STATUS_SUBSTITUTE(NotAuthorized,
          "User $0 has no $1 permission on <table $2.$3> or any of its parents",
          role_name, PermissionName(permission), keyspace, table);
    case ql::ObjectType::OBJECT_SCHEMA:
      if (canonical_resource == "data") {
        return STATUS_SUBSTITUTE(NotAuthorized,
            "User $0 has no $1 permission on <all keyspaces> or any of its parents",
            role_name, PermissionName(permission));
      }
      return STATUS_SUBSTITUTE(NotAuthorized,
          "User $0 has no $1 permission on <keyspace $2> or any of its parents",
          role_name, PermissionName(permission), keyspace);
    case ql::ObjectType::OBJECT_ROLE:
      if (canonical_resource == "role") {
        return STATUS_SUBSTITUTE(NotAuthorized,
            "User $0 has no $1 permission on <all roles> or any of its parents",
            role_name, PermissionName(permission));
      }
      return STATUS_SUBSTITUTE(NotAuthorized,
          "User $0 does not have sufficient privileges to perform the requested operation",
          role_name);
    default:
      return STATUS_SUBSTITUTE(IllegalState, "Unable to find permissions for object $0",
                               object_type);
  }
}

} // namespace

Status YBMetaDataCache::GetTable(const YBTableName& table_name,
                                 std::shared_ptr<YBTable>* table,
                                 bool* cache_used) {
  {
    std::lock_guard<std::mutex> lock(cached_tables_mutex_);
    auto itr = cached_tables_by_name_.find(table_name);
    if (itr != cached_tables_by_name_.end()) {
      *table = itr->second;
      *cache_used = true;
      return Status::OK();
    }
  }

  RETURN_NOT_OK(client_->OpenTable(table_name, table));
  {
    std::lock_guard<std::mutex> lock(cached_tables_mutex_);
    cached_tables_by_name_[(*table)->name()] = *table;
    cached_tables_by_id_[(*table)->id()] = *table;
  }
  *cache_used = false;
  return Status::OK();
}

Status YBMetaDataCache::GetTable(const TableId& table_id,
                                 std::shared_ptr<YBTable>* table,
                                 bool* cache_used) {
  {
    std::lock_guard<std::mutex> lock(cached_tables_mutex_);
    auto itr = cached_tables_by_id_.find(table_id);
    if (itr != cached_tables_by_id_.end()) {
      *table = itr->second;
      *cache_used = true;
      return Status::OK();
    }
  }

  RETURN_NOT_OK(client_->OpenTable(table_id, table));
  {
    std::lock_guard<std::mutex> lock(cached_tables_mutex_);
    cached_tables_by_name_[(*table)->name()] = *table;
    cached_tables_by_id_[table_id] = *table;
  }
  *cache_used = false;
  return Status::OK();
}

void YBMetaDataCache::RemoveCachedTable(const YBTableName& table_name) {
  std::lock_guard<std::mutex> lock(cached_tables_mutex_);
  const auto itr = cached_tables_by_name_.find(table_name);
  if (itr != cached_tables_by_name_.end()) {
    const auto table_id = itr->second->id();
    cached_tables_by_name_.erase(itr);
    cached_tables_by_id_.erase(table_id);
  }
}

void YBMetaDataCache::RemoveCachedTable(const TableId& table_id) {
  std::lock_guard<std::mutex> lock(cached_tables_mutex_);
  const auto itr = cached_tables_by_id_.find(table_id);
  if (itr != cached_tables_by_id_.end()) {
    const auto table_name = itr->second->name();
    cached_tables_by_name_.erase(table_name);
    cached_tables_by_id_.erase(itr);
  }
}

Status YBMetaDataCache::GetUDType(const string& keyspace_name,
                                  const string& type_name,
                                  std::shared_ptr<QLType> *type,
                                  bool *cache_used) {
  auto type_path = std::make_pair(keyspace_name, type_name);
  {
    std::lock_guard<std::mutex> lock(cached_types_mutex_);
    auto itr = cached_types_.find(type_path);
    if (itr != cached_types_.end()) {
      *type = itr->second;
      *cache_used = true;
      return Status::OK();
    }
  }

  RETURN_NOT_OK(client_->GetUDType(keyspace_name, type_name, type));
  {
    std::lock_guard<std::mutex> lock(cached_types_mutex_);
    cached_types_[type_path] = *type;
  }
  *cache_used = false;
  return Status::OK();
}

void YBMetaDataCache::RemoveCachedUDType(const string& keyspace_name,
                                         const string& type_name) {
  std::lock_guard<std::mutex> lock(cached_types_mutex_);
  cached_types_.erase(std::make_pair(keyspace_name, type_name));
}

Status YBMetaDataCache::WaitForPermissionCache() {
  if (!permissions_cache_) {
    LOG(WARNING) << "Permissions cache disabled. This only should be used in unit tests";
    return STATUS(TimedOut, "Permissions cache unavailable");
  }

  if (!permissions_cache_->ready()) {
    if (!permissions_cache_->WaitUntilReady(
            MonoDelta::FromMilliseconds(FLAGS_update_permissions_cache_msecs))) {
      return STATUS(TimedOut, "Permissions cache unavailable");
    }
  }
  return Status::OK();
}

Result<std::string> YBMetaDataCache::RoleSaltedHash(const RoleName& role_name) {
  RETURN_NOT_OK(WaitForPermissionCache());
  return permissions_cache_->salted_hash(role_name);
}

Result<bool> YBMetaDataCache::RoleCanLogin(const RoleName& role_name) {
  RETURN_NOT_OK(WaitForPermissionCache());
  return permissions_cache_->can_login(role_name);
}

Status YBMetaDataCache::HasResourcePermission(const std::string& canonical_resource,
                                              const ql::ObjectType& object_type,
                                              const RoleName& role_name,
                                              const PermissionType& permission,
                                              const NamespaceName& keyspace,
                                              const TableName& table,
                                              const internal::CacheCheckMode check_mode) {
  if (!permissions_cache_) {
    LOG(WARNING) << "Permissions cache disabled. This only should be used in unit tests";
    return Status::OK();
  }

  if (object_type != ql::ObjectType::OBJECT_SCHEMA &&
      object_type != ql::ObjectType::OBJECT_TABLE &&
      object_type != ql::ObjectType::OBJECT_ROLE) {
    DFATAL_OR_RETURN_NOT_OK(STATUS_SUBSTITUTE(InvalidArgument, "Invalid ObjectType $0",
                                              object_type));
  }

  if (!permissions_cache_->ready()) {
    if (!permissions_cache_->WaitUntilReady(
            MonoDelta::FromMilliseconds(FLAGS_update_permissions_cache_msecs))) {
      return STATUS(TimedOut, "Permissions cache unavailable");
    }
  }

  if (!permissions_cache_->HasCanonicalResourcePermission(canonical_resource, object_type,
                                                          role_name, permission)) {
    if (check_mode == internal::CacheCheckMode::RETRY) {
      // We could have failed to find the permission because our cache is stale. If we are asked
      // to retry, we update the cache and try again.
      RETURN_NOT_OK(client_->GetPermissions(permissions_cache_.get()));
      if (permissions_cache_->HasCanonicalResourcePermission(canonical_resource, object_type,
                                                             role_name, permission)) {
        return Status::OK();
      }
    }
    return GenerateUnauthorizedError(
        canonical_resource, object_type, role_name, permission, keyspace, table);
  }

  // Found.
  return Status::OK();
}

Status YBMetaDataCache::HasTablePermission(const NamespaceName& keyspace_name,
                                           const TableName& table_name,
                                           const RoleName& role_name,
                                           const PermissionType permission,
                                           const internal::CacheCheckMode check_mode) {

  // Check wihtout retry. In case our cache is stale, we will check again by issuing a recursive
  // call to this method.
  if (HasResourcePermission(get_canonical_keyspace(keyspace_name),
                            ql::ObjectType::OBJECT_SCHEMA, role_name, permission,
                            keyspace_name, "", internal::CacheCheckMode::NO_RETRY).ok()) {
    return Status::OK();
  }

  // By default the first call asks to retry. If we decide to retry, we will issue a recursive
  // call with NO_RETRY mode.
  Status s = HasResourcePermission(get_canonical_table(keyspace_name, table_name),
                                   ql::ObjectType::OBJECT_TABLE, role_name, permission,
                                   keyspace_name, table_name,
                                   check_mode);

  if (check_mode == internal::CacheCheckMode::RETRY && s.IsNotAuthorized()) {
    s = HasTablePermission(keyspace_name, table_name, role_name, permission,
                           internal::CacheCheckMode::NO_RETRY);
  }
  return s;
}

} // namespace client
} // namespace yb
