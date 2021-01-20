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

#include <atomic>

#include "yb/client/client.h"
#include "yb/client/permissions.h"

DECLARE_int32(update_permissions_cache_msecs);

namespace yb {

namespace client {

namespace internal {

using yb::master::GetPermissionsResponsePB;

RolePermissions::RolePermissions(const master::RolePermissionInfoPB& role_permission_info_pb) {
  DCHECK(role_permission_info_pb.has_all_keyspaces_permissions());
  DCHECK(role_permission_info_pb.has_all_roles_permissions());

  all_keyspaces_permissions_ = role_permission_info_pb.all_keyspaces_permissions();
  all_roles_permissions_ = role_permission_info_pb.all_roles_permissions();

  // For each resource, extract its permissions and store it in the role's permissions map.
  for (const auto &resource_permissions : role_permission_info_pb.resource_permissions()) {
    DCHECK(resource_permissions.has_permissions());
    VLOG(1) << "Processing permissions " << resource_permissions.ShortDebugString();

    auto it = resource_permissions_.find(resource_permissions.canonical_resource());
    if (it == resource_permissions_.end()) {
      resource_permissions_[resource_permissions.canonical_resource()] =
          resource_permissions.permissions();
    } else {
      // permissions is a bitmap representing the permissions.
      it->second |= resource_permissions.permissions();
    }
  }
}

bool RolePermissions::HasCanonicalResourcePermission(const std::string& canonical_resource,
                                                     PermissionType permission) const {
  const auto& resource_permissions_itr = resource_permissions_.find(canonical_resource);
  if (resource_permissions_itr == resource_permissions_.end()) {
    return false;
  }
  const Permissions& resource_permissions_bitset = resource_permissions_itr->second;
  return resource_permissions_bitset.test(permission);
}

bool RolePermissions::HasAllKeyspacesPermission(PermissionType permission) const {
  return all_keyspaces_permissions_.test(permission);
}

bool RolePermissions::HasAllRolesPermission(PermissionType permission) const {
  return all_roles_permissions_.test(permission);
}

PermissionsCache::PermissionsCache(client::YBClient* client,
                                   bool automatically_update_cache) : client_(client) {
  if (!automatically_update_cache) {
    return;
  }
  LOG(INFO) << "Creating permissions cache";
  pool_ = std::make_unique<yb::rpc::IoThreadPool>("permissions_cache_updater", 1);
  scheduler_ = std::make_unique<yb::rpc::Scheduler>(&pool_->io_service());
  // This will send many concurrent requests to the master for the permission data.
  // Queries done before a master leader is elected are all going to fail. This shouldn't be
  // an issue if the default refresh value is low enough.
  // TODO(hector): Add logic to retry failed requests so we don't depend on automatic
  // rescheduling to refresh the permissions cache.
  ScheduleGetPermissionsFromMaster(true);
}

PermissionsCache::~PermissionsCache() {
  if (pool_) {
    scheduler_->Shutdown();
    pool_->Shutdown();
    pool_->Join();
  }
}

bool PermissionsCache::WaitUntilReady(MonoDelta wait_for) {
  std::unique_lock<std::mutex> l(mtx_);
  return cond_.wait_for(l, wait_for.ToSteadyDuration(),
                        [this] { return this->ready_.load(std::memory_order_acquire); });
}

void PermissionsCache::ScheduleGetPermissionsFromMaster(bool now) {
  if (FLAGS_update_permissions_cache_msecs <= 0) {
    return;
  }

  DCHECK(pool_);
  DCHECK(scheduler_);

  scheduler_->Schedule([this](const Status &s) {
    if (!s.ok()) {
      LOG(INFO) << "Permissions cache updater scheduler was shutdown: " << s.ToString();
      return;
    }
    this->GetPermissionsFromMaster();
  }, std::chrono::milliseconds(now ? 0 : FLAGS_update_permissions_cache_msecs));
}

void PermissionsCache::UpdateRolesPermissions(const GetPermissionsResponsePB& resp) {
  auto new_roles_permissions_map = std::make_shared<RolesPermissionsMap>();
  auto new_roles_auth_info_map = std::make_shared<RolesAuthInfoMap>();

  // Populate the cache.
  // Get all the roles in the response. They should have at least one piece of information:
  // the permissions for 'ALL ROLES' and 'ALL KEYSPACES'
  for (const auto& role_permissions : resp.role_permissions()) {
    auto result = new_roles_permissions_map->emplace(role_permissions.role(),
                                                     RolePermissions(role_permissions));
    LOG_IF(DFATAL, !result.second) << "Error inserting permissions for role "
                                   << role_permissions.role();

    RoleAuthInfo role_auth_info;
    role_auth_info.salted_hash = role_permissions.salted_hash();
    role_auth_info.can_login = role_permissions.can_login();
    auto result2 = new_roles_auth_info_map->emplace(role_permissions.role(),
                                                    std::move(role_auth_info));
    LOG_IF(DFATAL, !result2.second) << "Error inserting authentication info for role "
                                   << role_permissions.role();
  }

  {
    std::unique_lock<simple_spinlock> l(permissions_cache_lock_);
    // It's possible that another thread already updated the cache with a more recent version.
    if (version_ < resp.version()) {
      std::atomic_store_explicit(&roles_permissions_map_, std::move(new_roles_permissions_map),
          std::memory_order_release);
      std::atomic_store_explicit(&roles_auth_info_map_, std::move(new_roles_auth_info_map),
          std::memory_order_release);
      // Set the permissions cache's version.
      version_ = resp.version();
    }
  }
  {
    // We need to hold the mutex before modifying ready_ to avoid a race condition with cond_.
    std::lock_guard<std::mutex> l(mtx_);
    ready_.store(true, std::memory_order_release);
  }
  cond_.notify_all();
}

void PermissionsCache::GetPermissionsFromMaster() {
  // Schedule the cache update before we start processing anything because we want to stay as close
  // as possible to the refresh rate specified by the update_permissions_cache_msecs flag.
  // TODO(hector): Add a variable to track the last time that the cache was updated and have a
  // metric exposed for it, per-server.
  ScheduleGetPermissionsFromMaster(false);

  Status s = client_->GetPermissions(this);

  if (!s.ok()) {
    LOG(WARNING) << "Unable to refresh permissions cache. Received error: " << s.ToString();
    // TODO(hector): If we received an error, then our cache will become stale. We need to allow
    // users to specify the max staleness that they are willing to tolerate.
    // For now it's safe to ignore the error since we always check
  }
}

Result<std::string> PermissionsCache::salted_hash(const RoleName& role_name) {
  std::shared_ptr<RolesAuthInfoMap> roles_auth_info_map;
  roles_auth_info_map = std::atomic_load_explicit(&roles_auth_info_map_,
    std::memory_order_acquire);
  auto it = roles_auth_info_map->find(role_name);
  if (it == roles_auth_info_map->end()) {
    return STATUS(NotFound, "Role not found");
  }
  return it->second.salted_hash;
}

Result<bool> PermissionsCache::can_login(const RoleName& role_name) {
  std::shared_ptr<RolesAuthInfoMap> roles_auth_info_map;
  roles_auth_info_map = std::atomic_load_explicit(&roles_auth_info_map_,
    std::memory_order_acquire);
  auto it = roles_auth_info_map->find(role_name);
  if (it == roles_auth_info_map->end()) {
    return STATUS(NotFound, "Role not found");
  }
  return it->second.can_login;
}

bool PermissionsCache::HasCanonicalResourcePermission(const std::string& canonical_resource,
                                                      const ql::ObjectType& object_type,
                                                      const RoleName& role_name,
                                                      const PermissionType& permission) {
  std::shared_ptr<RolesPermissionsMap> roles_permissions_map;
  roles_permissions_map = std::atomic_load_explicit(&roles_permissions_map_,
      std::memory_order_acquire);

  const auto& role_permissions_iter = roles_permissions_map->find(role_name);
  if (role_permissions_iter == roles_permissions_map->end()) {
    VLOG(1) << "Role " << role_name << " not found";
    // Role doesn't exist.
    return false;
  }

  // Check if the requested permission has been granted to 'ALL KEYSPACES' or to 'ALL ROLES'.
  const auto& role_permissions = role_permissions_iter->second;
  if (object_type == ql::ObjectType::OBJECT_SCHEMA || object_type == ql::ObjectType::OBJECT_TABLE) {
    if (role_permissions.HasAllKeyspacesPermission(permission)) {
      // Found.
      return true;
    }
  } else if (object_type == ql::ObjectType::OBJECT_ROLE) {
    if (role_permissions.HasAllRolesPermission(permission)) {
      // Found.
      return true;
    }
  }

  // If we didn't find the permission by checking all_permissions, then the queried permission was
  // not granted to 'ALL KEYSPACES' or to 'ALL ROLES'.
  if (canonical_resource == kRolesDataResource || canonical_resource == kRolesRoleResource) {
    return false;
  }

  return role_permissions.HasCanonicalResourcePermission(canonical_resource, permission);
}


} // namespace namespace internal
} // namespace client
} // namespace yb
