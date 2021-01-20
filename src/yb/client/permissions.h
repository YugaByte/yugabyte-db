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

#ifndef YB_CLIENT_PERMISSIONS_H
#define YB_CLIENT_PERMISSIONS_H

#include <boost/optional.hpp>

#include "yb/common/entity_ids.h"
#include "yb/common/roles_permissions.h"
#include "yb/master/master.pb.h"
#include "yb/util/locks.h"
#include "yb/util/monotime.h"
#include "yb/rpc/io_thread_pool.h"
#include "yb/rpc/scheduler.h"
#include "yb/yql/cql/ql/ptree/pt_option.h"

namespace yb {

namespace client {

class YBClient;

namespace internal {

class RolePermissions {
 public:
  explicit RolePermissions(const master::RolePermissionInfoPB& role_permission_info_pb);

  bool HasCanonicalResourcePermission(const std::string &canonical_resource,
                                      PermissionType permission) const;

  bool HasAllKeyspacesPermission(PermissionType permission) const;

  bool HasAllRolesPermission(PermissionType permission) const;

  const Permissions& all_keyspaces_permissions() const {
    return all_keyspaces_permissions_;
  }

  const Permissions& all_roles_permissions() const {
    return all_roles_permissions_;
  }

  const std::unordered_map<std::string, Permissions>& resource_permissions() const {
    return resource_permissions_;
  }

 private:

  // Permissions for resources "ALL KEYSPACES" ('data') and "ALL ROLES" ('roles').
  // Special case to avoid hashing canonical resources 'data' and 'roles' and doing a hashmap
  // lookup.
  Permissions all_keyspaces_permissions_;
  Permissions all_roles_permissions_;
  // canonical resource -> permission bitmap.
  std::unordered_map<std::string, Permissions> resource_permissions_;
};

struct RoleAuthInfo {
  std::string salted_hash;
  bool can_login;
};

using RolesPermissionsMap = std::unordered_map<RoleName, RolePermissions>;
using RolesAuthInfoMap = std::unordered_map<RoleName, RoleAuthInfo>;

enum class CacheCheckMode {
  NO_RETRY,
  RETRY,
};

class PermissionsCache {
 public:
  explicit PermissionsCache(client::YBClient* client,
                            bool automatically_update_cache = true);

  ~PermissionsCache();

  void UpdateRolesPermissions(const master::GetPermissionsResponsePB& resp);

  std::shared_ptr<RolesPermissionsMap> get_roles_permissions_map() {
    std::unique_lock<simple_spinlock> l(permissions_cache_lock_);
    return roles_permissions_map_;
  }

  bool ready() {
    return ready_.load(std::memory_order_acquire);
  }

  // Wait until the cache is ready (it has received at least one update from the master). Returns
  // false if the cache is not ready after waiting for the specified time. Returns true otherwise.
  bool WaitUntilReady(MonoDelta wait_for);

  boost::optional<uint64_t> version() {
    std::unique_lock<simple_spinlock> l(permissions_cache_lock_);
    return version_;
  }

  bool HasCanonicalResourcePermission(const std::string &canonical_resource,
                                      const ql::ObjectType &object_type,
                                      const RoleName &role_name,
                                      const PermissionType &permission);

  Result<std::string> salted_hash(const RoleName& role_name);
  Result<bool> can_login(const RoleName& role_name);

 private:
  void ScheduleGetPermissionsFromMaster(bool now);

  void GetPermissionsFromMaster();

  // Passed to the master whenever we want to update our cache. The master will only send a new
  // cache if its version number is greater than this version number.
  boost::optional<uint64_t> version_;

  // Client used to send the request to the master.
  client::YBClient* const client_;

  // role name -> RolePermissions.
  std::shared_ptr<RolesPermissionsMap> roles_permissions_map_;
  std::shared_ptr<RolesAuthInfoMap> roles_auth_info_map_;

  // Used to modify the internal state.
  mutable simple_spinlock permissions_cache_lock_;

  // Used to wait until the cache is ready.
  std::mutex mtx_;
  std::condition_variable cond_;

  // Thread pool used by the scheduler.
  std::unique_ptr<yb::rpc::IoThreadPool> pool_;
  // The scheduler used to refresh the permissions.
  std::unique_ptr<yb::rpc::Scheduler> scheduler_;

  // Whether we have received the permissions from the master.
  std::atomic<bool> ready_{false};
};

} // namespace namespace internal
} // namespace client
} // namespace yb

#endif // YB_CLIENT_PERMISSIONS_H
