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

#include <mutex>

#include "yb/util/crypt.h"

#include "yb/gutil/strings/substitute.h"

#include "yb/master/catalog_manager.h"
#include "yb/master/catalog_manager-internal.h"
#include "yb/master/master_util.h"
#include "yb/master/permissions_manager.h"
#include "yb/master/sys_catalog.h"
#include "yb/master/sys_catalog_constants.h"

#include "yb/util/shared_lock.h"

using std::shared_ptr;

using yb::util::kBcryptHashSize;
using yb::util::bcrypt_hashpw;
using strings::Substitute;

// TODO: remove direct references to member fields in CatalogManager from here.

namespace yb {
namespace master {

namespace {

// Helper class to abort mutations at the end of a scope.
template<class PersistentDataEntryPB>
class ScopedMutation {
 public:
  explicit ScopedMutation(PersistentDataEntryPB* cow_object)
      : cow_object_(DCHECK_NOTNULL(cow_object)) {
    cow_object->mutable_metadata()->StartMutation();
  }

  void Commit() {
    cow_object_->mutable_metadata()->CommitMutation();
    committed_ = true;
  }

  // Abort the mutation if it wasn't committed.
  ~ScopedMutation() {
    if (PREDICT_FALSE(!committed_)) {
      cow_object_->mutable_metadata()->AbortMutation();
    }
}

 private:
  PersistentDataEntryPB* cow_object_;
  bool committed_ = false;
};

}  // anonymous namespace


PermissionsManager::PermissionsManager(CatalogManager* catalog_manager)
    : catalog_manager_(catalog_manager) {
  CHECK_NOTNULL(catalog_manager);
}

Status PermissionsManager::PrepareDefaultRoles(int64_t term) {
  // Verify we have the catalog manager lock.
  if (!catalog_manager_->lock_.is_locked()) {
    return STATUS(IllegalState, "We don't have the catalog manager lock!");
  }

  if (FindPtrOrNull(roles_map_, kDefaultCassandraUsername) != nullptr) {
    LOG(INFO) << "Role " << kDefaultCassandraUsername
              << " already created, skipping initialization";
    return Status::OK();
  }

  char hash[kBcryptHashSize];
  // TODO: refactor interface to be more c++ like...
  int ret = bcrypt_hashpw(kDefaultCassandraPassword, hash);
  if (ret != 0) {
    return STATUS_SUBSTITUTE(IllegalState, "Could not hash password, reason: $0", ret);
  }

  // Create in memory object.
  Status s = CreateRoleUnlocked(kDefaultCassandraUsername, std::string(hash, kBcryptHashSize),
                                true, true, term, false /* Don't increment the roles version */);
  if (PREDICT_TRUE(s.ok())) {
    LOG(INFO) << "Created role: " << kDefaultCassandraUsername;
  }

  return s;
}

template<class RespClass>
Status PermissionsManager::GrantPermissions(
    const RoleName& role_name,
    const std::string& canonical_resource,
    const std::string& resource_name,
    const NamespaceName& keyspace,
    const std::vector<PermissionType>& permissions,
    const ResourceType resource_type,
    RespClass* resp) {
  std::lock_guard<decltype(catalog_manager_->lock_)> l(catalog_manager_->lock_);

  scoped_refptr<RoleInfo> rp;
  rp = FindPtrOrNull(roles_map_, role_name);
  if (rp == nullptr) {
    const Status s = STATUS_SUBSTITUTE(NotFound, "Role $0 was not found", role_name);
    return SetupError(resp->mutable_error(), MasterErrorPB::ROLE_NOT_FOUND, s);
  }

  RETURN_NOT_OK(IncrementRolesVersionUnlocked());

  {
    SysRoleEntryPB* metadata;
    ScopedMutation <RoleInfo> role_info_mutation(rp.get());
    metadata = &rp->mutable_metadata()->mutable_dirty()->pb;

    ResourcePermissionsPB* current_resource = metadata->add_resources();

    current_resource->set_canonical_resource(canonical_resource);
    current_resource->set_resource_type(resource_type);
    current_resource->set_resource_name(resource_name);
    current_resource->set_namespace_name(keyspace);

    for (const auto& permission : permissions) {
      if (permission == PermissionType::DESCRIBE_PERMISSION &&
          resource_type != ResourceType::ROLE &&
          resource_type != ResourceType::ALL_ROLES) {
        // Describe permission should only be granted to the role resource.
        continue;
      }
      current_resource->add_permissions(permission);
    }
    Status s = catalog_manager_->sys_catalog_->UpdateItem(rp.get(),
        catalog_manager_->leader_ready_term());
    if (!s.ok()) {
      s = s.CloneAndPrepend(Substitute(
          "An error occurred while updating permissions in sys-catalog: $0", s.ToString()));
      LOG(WARNING) << s;
      return CheckIfNoLongerLeaderAndSetupError(s, resp);
    }
    TRACE("Wrote Permission to sys-catalog");
    role_info_mutation.Commit();

    BuildResourcePermissionsUnlocked();
  }
  return Status::OK();
}

// TODO: get rid of explicit instantiations.
template Status PermissionsManager::GrantPermissions<CreateTableResponsePB>(
    const RoleName& role_name,
    const std::string& canonical_resource,
    const std::string& resource_name,
    const NamespaceName& keyspace,
    const std::vector<PermissionType>& permissions,
    const ResourceType resource_type,
    CreateTableResponsePB* resp);

template Status PermissionsManager::GrantPermissions<CreateNamespaceResponsePB>(
    const RoleName& role_name,
    const std::string& canonical_resource,
    const std::string& resource_name,
    const NamespaceName& keyspace,
    const std::vector<PermissionType>& permissions,
    const ResourceType resource_type,
    CreateNamespaceResponsePB* resp);

// Create a SysVersionInfo object to track the roles versions.
Status PermissionsManager::IncrementRolesVersionUnlocked() {
  DCHECK(catalog_manager_->lock_.is_locked()) << "We don't have the catalog manager lock!";

  // Prepare write.
  auto l = CHECK_NOTNULL(security_config_.get())->LockForWrite();
  const uint64_t roles_version = l->mutable_data()->pb.security_config().roles_version();
  if (roles_version == std::numeric_limits<uint64_t>::max()) {
    DFATAL_OR_RETURN_NOT_OK(
        STATUS_SUBSTITUTE(IllegalState,
                          "Roles version reached max allowable integer: $0", roles_version));
  }
  l->mutable_data()->pb.mutable_security_config()->set_roles_version(roles_version + 1);

  TRACE("Set CatalogManager's roles version");

  // Write to sys_catalog and in memory.
  RETURN_NOT_OK(catalog_manager_->sys_catalog_->UpdateItem(
      security_config_.get(), catalog_manager_->leader_ready_term()));

  l->Commit();
  return Status::OK();
}

template<class RespClass>
Status PermissionsManager::RemoveAllPermissionsForResourceUnlocked(
    const std::string& canonical_resource,
    RespClass* resp) {

  DCHECK(catalog_manager_->lock_.is_locked()) << "We don't have the catalog manager lock!";

  bool permissions_modified = false;
  for (const auto& e : roles_map_) {
    scoped_refptr<RoleInfo> rp = e.second;
    ScopedMutation<RoleInfo> role_info_mutation(rp.get());
    auto* resources = rp->mutable_metadata()->mutable_dirty()->pb.mutable_resources();
    for (auto itr = resources->begin(); itr != resources->end(); itr++) {
      if (itr->canonical_resource() == canonical_resource) {
        resources->erase(itr);
        role_info_mutation.Commit();
        permissions_modified = true;
        break;
      }
    }
  }

  // Increment the roles version and update the cache only if there was a modification to the
  // permissions.
  if (permissions_modified) {
    const Status s = IncrementRolesVersionUnlocked();
    if (!s.ok()) {
      return CheckIfNoLongerLeaderAndSetupError(s, resp);
    }

    BuildResourcePermissionsUnlocked();
  }

  return Status::OK();
}

template<class RespClass>
Status PermissionsManager::RemoveAllPermissionsForResource(
    const std::string& canonical_resource,
    RespClass* resp) {
  std::lock_guard<decltype(catalog_manager_->lock_)> l_big(catalog_manager_->lock_);
  return RemoveAllPermissionsForResourceUnlocked(canonical_resource, resp);
}

// TODO: get rid of the need for explicit instantations.
template
Status PermissionsManager::RemoveAllPermissionsForResource<DeleteTableResponsePB>(
    const std::string& canonical_resource,
    DeleteTableResponsePB* resp);

template
Status PermissionsManager::RemoveAllPermissionsForResource<DeleteNamespaceResponsePB>(
    const std::string& canonical_resource,
    DeleteNamespaceResponsePB* resp);

Status PermissionsManager::CreateRoleUnlocked(
    const std::string& role_name,
    const std::string& salted_hash,
    const bool login,
    const bool superuser,
    int64_t term,
    const bool increment_roles_version) {
  if (!catalog_manager_->lock_.is_locked()) {
    return STATUS(IllegalState, "We don't have the catalog manager lock!");
  }
  // Create Entry.
  SysRoleEntryPB role_entry;
  role_entry.set_role(role_name);
  role_entry.set_can_login(login);
  role_entry.set_is_superuser(superuser);
  if (salted_hash.size() != 0) {
    role_entry.set_salted_hash(salted_hash);
  }

  if (increment_roles_version) {
    // Increment roles version.
    RETURN_NOT_OK(IncrementRolesVersionUnlocked());
  }

  // Create in memory object.
  scoped_refptr<RoleInfo> role = new RoleInfo(role_name);

  // Prepare write.
  auto l = role->LockForWrite();
  l->mutable_data()->pb = std::move(role_entry);

  roles_map_[role_name] = role;
  TRACE("Inserted new role info into CatalogManager maps");

  // Write to sys_catalog and in memory.
  RETURN_NOT_OK(catalog_manager_->sys_catalog_->AddItem(role.get(), term));

  l->Commit();
  BuildRecursiveRolesUnlocked();
  return Status::OK();
}

Status PermissionsManager::CreateRole(
    const CreateRoleRequestPB* req,
    CreateRoleResponsePB* resp,
    rpc::RpcContext* rpc) {

  LOG(INFO) << "CreateRole from " << RequestorString(rpc) << ": " << req->DebugString();

  RETURN_NOT_OK(catalog_manager_->CheckOnline());
  Status s;
  {
    TRACE("Acquired catalog manager lock");
    std::lock_guard<decltype(catalog_manager_->lock_)> l_big(catalog_manager_->lock_);
    // Only a SUPERUSER role can create another SUPERUSER role. In Apache Cassandra this gets
    // checked before the existence of the new role.
    if (req->superuser()) {
      scoped_refptr<RoleInfo> creator_role = FindPtrOrNull(roles_map_, req->creator_role_name());
      if (creator_role == nullptr) {
        s = STATUS_SUBSTITUTE(NotFound, "role $0 does not exist", req->creator_role_name());
        return SetupError(resp->mutable_error(), MasterErrorPB::ROLE_NOT_FOUND, s);
      }

      auto clr = creator_role->LockForRead();
      if (!clr->data().pb.is_superuser()) {
        s = STATUS(NotAuthorized, "Only superusers can create a role with superuser status");
        return SetupError(resp->mutable_error(), MasterErrorPB::NOT_AUTHORIZED, s);
      }
    }
    if (FindPtrOrNull(roles_map_, req->name()) != nullptr) {
      s = STATUS_SUBSTITUTE(AlreadyPresent, "Role $0 already exists", req->name());
      return SetupError(resp->mutable_error(), MasterErrorPB::ROLE_ALREADY_PRESENT, s);
    }
    s = CreateRoleUnlocked(
        req->name(), req->salted_hash(), req->login(), req->superuser(),
        catalog_manager_->leader_ready_term());
  }
  if (PREDICT_TRUE(s.ok())) {
    LOG(INFO) << "Created role: " << req->name();
    if (req->has_creator_role_name()) {
      RETURN_NOT_OK(GrantPermissions(req->creator_role_name(),
                                     get_canonical_role(req->name()),
                                     req->name() /* resource name */,
                                     "" /* keyspace name */,
                                     all_permissions_for_resource(ResourceType::ROLE),
                                     ResourceType::ROLE,
                                     resp));
    }
  }
  return s;
}

Status PermissionsManager::AlterRole(
    const AlterRoleRequestPB* req,
    AlterRoleResponsePB* resp,
    rpc::RpcContext* rpc) {

  VLOG(1) << "AlterRole from " << RequestorString(rpc) << ": " << req->DebugString();

  RETURN_NOT_OK(catalog_manager_->CheckOnline());
  Status s;

  TRACE("Acquired catalog manager lock");
  std::lock_guard<decltype(catalog_manager_->lock_)> l_big(catalog_manager_->lock_);

  auto role = FindPtrOrNull(roles_map_, req->name());
  if (role == nullptr) {
    s = STATUS_SUBSTITUTE(NotFound, "Role $0 does not exist", req->name());
    return SetupError(resp->mutable_error(), MasterErrorPB::ROLE_NOT_FOUND, s);
  }

  // Increment roles version.
  RETURN_NOT_OK(IncrementRolesVersionUnlocked());

  // Modify the role.
  auto l = role->LockForWrite();

  // If the role we are trying to alter is a SUPERUSER, and the request is trying to alter the
  // SUPERUSER field for that role, the role requesting the alter operation must be a SUPERUSER
  // too.
  if (req->has_superuser()) {
    auto current_role = FindPtrOrNull(roles_map_, req->current_role());
    if (current_role == nullptr) {
      s = STATUS_SUBSTITUTE(NotFound, "Internal error: role $0 does not exist",
                            req->current_role());
      return SetupError(resp->mutable_error(), MasterErrorPB::ROLE_NOT_FOUND, s);
    }

    // Fix for https://github.com/yugabyte/yugabyte-db/issues/2505.
    // A role cannot modify its own superuser status, nor the superuser status of any role granted
    // to it directly or through inheritance. This check should happen before the next check that
    // verifies that the role requesting the modification is a superuser.
    if (l->data().pb.role() == req->current_role() ||
        IsMemberOf(l->data().pb.role(), req->current_role())) {
      s = STATUS(NotAuthorized,
          "You aren't allowed to alter your own superuser status or that of a role granted to you");
      return SetupError(resp->mutable_error(), MasterErrorPB::NOT_AUTHORIZED, s);
    }

    // Don't allow a non-superuser role to modify the superuser status of another role.
    auto clr = current_role->LockForRead();
    if (!clr->data().pb.is_superuser()) {
      s = STATUS(NotAuthorized, "Only superusers are allowed to alter superuser status");
      return SetupError(resp->mutable_error(), MasterErrorPB::NOT_AUTHORIZED, s);
    }
  }

  if (req->has_login()) {
    l->mutable_data()->pb.set_can_login(req->login());
  }
  if (req->has_superuser()) {
    l->mutable_data()->pb.set_is_superuser(req->superuser());
  }
  if (req->has_salted_hash()) {
    l->mutable_data()->pb.set_salted_hash(req->salted_hash());
  }

  s = catalog_manager_->sys_catalog_->UpdateItem(role.get(),
                                                 catalog_manager_->leader_ready_term());
  if (!s.ok()) {
    LOG(ERROR) << "Unable to alter role " << req->name() << ": " << s;
    return s;
  }
  l->Commit();
  VLOG(1) << "Altered role with request: " << req->ShortDebugString();
  if (req->has_superuser()) {
    BuildResourcePermissionsUnlocked();
  }
  return Status::OK();
}

Status PermissionsManager::DeleteRole(
    const DeleteRoleRequestPB* req,
    DeleteRoleResponsePB* resp,
    rpc::RpcContext* rpc) {

  LOG(INFO) << "Servicing DeleteRole request from " << RequestorString(rpc)
            << ": " << req->ShortDebugString();
  RETURN_NOT_OK(catalog_manager_->CheckOnline());
  Status s;

  if (!req->has_name()) {
    s = STATUS(InvalidArgument, "No role name given", req->DebugString());
    return SetupError(resp->mutable_error(), MasterErrorPB::ROLE_NOT_FOUND, s);
  }

  TRACE("Acquired catalog manager lock");
  std::lock_guard<decltype(catalog_manager_->lock_)> l_big(catalog_manager_->lock_);

  auto role = FindPtrOrNull(roles_map_, req->name());
  if (role == nullptr) {
    s = STATUS_SUBSTITUTE(NotFound, "Role $0 does not exist", req->name());
    return SetupError(resp->mutable_error(), MasterErrorPB::ROLE_NOT_FOUND, s);
  }

  // Increment roles version.
  RETURN_NOT_OK(IncrementRolesVersionUnlocked());

  // Find all the roles where req->name() is part of the member_of list since we will need to remove
  // the role we are deleting from those lists.
  auto direct_member_of = DirectMemberOf(req->name());
  for (const auto& role_name : direct_member_of) {
    auto role = FindPtrOrNull(roles_map_, role_name);
    if (role == nullptr) {
      continue;
    }
    role->mutable_metadata()->StartMutation();
    auto metadata = &role->mutable_metadata()->mutable_dirty()->pb;

    // Create a new list that contains all the original roles in member_of with the exception of
    // the role we are deleting.
    vector<string> member_of_new_list;
    for (const auto& member_of : metadata->member_of()) {
      if (member_of != req->name()) {
        member_of_new_list.push_back(member_of);
      }
    }

    // Remove the role we are deleting from the list member_of.
    metadata->clear_member_of();
    for (auto member_of : member_of_new_list) {
      metadata->add_member_of(std::move(member_of));
    }

    // Update sys-catalog with the new member_of list for this role.
    s = catalog_manager_->sys_catalog_->UpdateItem(role.get(),
        catalog_manager_->leader_ready_term());
    if (!s.ok()) {
      LOG(ERROR) << "Unable to remove role " << req->name()
                 << " from member_of list for role " << role_name;
      role->mutable_metadata()->AbortMutation();
    } else {
      role->mutable_metadata()->CommitMutation();
    }
  }

  auto l = role->LockForWrite();

  if (l->mutable_data()->pb.is_superuser()) {
    // If the role we are trying to remove is a SUPERUSER, the role trying to remove it has to be
    // a SUPERUSER too.
    auto current_role = FindPtrOrNull(roles_map_, req->current_role());
    if (current_role == nullptr) {
      s = STATUS_SUBSTITUTE(NotFound, "Internal error: role $0 does not exist",
                            req->current_role());
      return SetupError(resp->mutable_error(), MasterErrorPB::ROLE_NOT_FOUND, s);
    }

    auto clr = current_role->LockForRead();
    if (!clr->data().pb.is_superuser()) {
      s = STATUS(NotAuthorized, "Only superusers can drop a role with superuser status");
      return SetupError(resp->mutable_error(), MasterErrorPB::NOT_AUTHORIZED, s);
    }
  }

  // Write to sys_catalog and in memory.
  RETURN_NOT_OK(catalog_manager_->sys_catalog_->DeleteItem(role.get(),
      catalog_manager_->leader_ready_term()));
  // Remove it from the maps.
  if (roles_map_.erase(role->id()) < 1) {
    PANIC_RPC(rpc, "Could not remove role from map, role name=" + role->id());
  }

  // Update the in-memory state.
  TRACE("Committing in-memory state");
  l->Commit();
  BuildRecursiveRolesUnlocked();

  // Remove all the permissions granted on the deleted role to any role. See DeleteTable() comment
  // for for more details.
  string canonical_resource = get_canonical_role(req->name());
  RETURN_NOT_OK(RemoveAllPermissionsForResourceUnlocked(canonical_resource, resp));

  LOG(INFO) << "Successfully deleted role " << role->ToString()
            << " per request from " << RequestorString(rpc);

  return Status::OK();
}

Status PermissionsManager::GrantRevokeRole(
    const GrantRevokeRoleRequestPB* req,
    GrantRevokeRoleResponsePB* resp,
    rpc::RpcContext* rpc) {

  LOG(INFO) << "Servicing " << (req->revoke() ? "RevokeRole" : "GrantRole")
            << " request from " << RequestorString(rpc) << ": " << req->ShortDebugString();
  RETURN_NOT_OK(catalog_manager_->CheckOnline());

  // Cannot grant or revoke itself.
  if (req->granted_role() == req->recipient_role()) {
    if (req->revoke()) {
      // Ignore the request. This is what Apache Cassandra does.
      return Status::OK();
    }
    auto s = STATUS_SUBSTITUTE(InvalidArgument,
        "$0 is a member of $1", req->recipient_role(), req->granted_role());
    return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_REQUEST, s);
  }

  Status s;
  // If the request is revoke, we need to create a new list of the roles req->recipient_role()
  // is member of in which we exclude req->granted_role().
  if (!req->has_granted_role() || !req->has_recipient_role()) {
    s = STATUS(InvalidArgument, "No role name given", req->DebugString());
    return SetupError(resp->mutable_error(), MasterErrorPB::ROLE_NOT_FOUND, s);
  }

  {
    constexpr char role_not_found_msg_str[] = "$0 doesn't exist";
    TRACE("Acquired catalog manager lock");
    std::lock_guard<decltype(catalog_manager_->lock_)> l_big(catalog_manager_->lock_);

    scoped_refptr<RoleInfo> granted_role;
    granted_role = FindPtrOrNull(roles_map_, req->granted_role());
    if (granted_role == nullptr) {
      s = STATUS_SUBSTITUTE(NotFound, role_not_found_msg_str, req->granted_role());
      return SetupError(resp->mutable_error(), MasterErrorPB::ROLE_NOT_FOUND, s);
    }

    scoped_refptr<RoleInfo> recipient_role;
    recipient_role = FindPtrOrNull(roles_map_, req->recipient_role());
    if (recipient_role == nullptr) {
      s = STATUS_SUBSTITUTE(NotFound, role_not_found_msg_str, req->recipient_role());
      return SetupError(resp->mutable_error(), MasterErrorPB::ROLE_NOT_FOUND, s);
    }

    // Both roles are present.
    SysRoleEntryPB* metadata;
    {
      ScopedMutation<RoleInfo> role_info_mutation(recipient_role.get());
      metadata = &recipient_role->mutable_metadata()->mutable_dirty()->pb;

      // When revoking a role, the granted role has to be a direct member of the recipient role,
      // but when granting a role, if the recipient role has been granted to the granted role either
      // directly or through inheritance, we will return an error.
      if (req->revoke()) {
        bool direct_member = false;
        vector<string> member_of_new_list;
        for (const auto& member_of : metadata->member_of()) {
          if (member_of == req->granted_role()) {
            direct_member = true;
          } else if (req->revoke()) {
            member_of_new_list.push_back(member_of);
          }
        }

        if (!direct_member) {
          s = STATUS_SUBSTITUTE(InvalidArgument, "$0 is not a member of $1",
                                req->recipient_role(), req->granted_role());
          return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_REQUEST, s);
        }

        metadata->clear_member_of();
        for (auto member_of : member_of_new_list) {
          metadata->add_member_of(std::move(member_of));
        }
        s = catalog_manager_->sys_catalog_->UpdateItem(recipient_role.get(),
            catalog_manager_->leader_ready_term());
      } else {
        // Let's make sure that we don't have circular dependencies.
        if (IsMemberOf(req->granted_role(), req->recipient_role()) ||
            IsMemberOf(req->recipient_role(), req->granted_role()) ||
            req->granted_role() == req->recipient_role()) {
          s = STATUS_SUBSTITUTE(InvalidArgument, "$0 is a member of $1",
                                req->recipient_role(), req->granted_role());
          return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_REQUEST, s);
        }
        metadata->add_member_of(req->granted_role());
        s = catalog_manager_->sys_catalog_->UpdateItem(recipient_role.get(),
            catalog_manager_->leader_ready_term());
      }
      if (!s.ok()) {
        s = s.CloneAndPrepend(Substitute(
            "An error occurred while updating roles in sys-catalog: $0", s.ToString()));
        LOG(WARNING) << s.ToString();
        return CheckIfNoLongerLeaderAndSetupError(s, resp);
      }

      LOG(INFO) << "Modified 'member of' field of role " << recipient_role->id();

      // Increment roles version before commiting the mutation.
      RETURN_NOT_OK(IncrementRolesVersionUnlocked());

      role_info_mutation.Commit();
      BuildRecursiveRolesUnlocked();
    }
    TRACE("Wrote grant/revoke role to sys-catalog");
  }

  return Status::OK();
}

// Depth first search. We assume that there are no cycles in the graph of dependencies.
void PermissionsManager::BuildRecursiveRolesUnlocked() {
  recursive_granted_roles_.clear();

  // Build the first level member of map and find all the roles that have at least one member in its
  // member_of field.
  for (const auto& e : roles_map_) {
    const auto& role_name = e.first;
    if (recursive_granted_roles_.find(role_name) == recursive_granted_roles_.end()) {
      TraverseRole(role_name, nullptr);
    }
  }

  BuildResourcePermissionsUnlocked();
}

void PermissionsManager::BuildResourcePermissionsUnlocked() {
  shared_ptr<GetPermissionsResponsePB> response = std::make_shared<GetPermissionsResponsePB>();

  for (const auto& e : recursive_granted_roles_) {
    const auto& role_name = e.first;
    // Get a copy of this set.
    auto granted_roles = e.second;
    granted_roles.insert(role_name);
    auto* role_permissions = response->add_role_permissions();
    role_permissions->set_role(role_name);

    // No permissions on ALL ROLES and ALL KEYSPACES by default.
    role_permissions->set_all_keyspaces_permissions(0);
    role_permissions->set_all_roles_permissions(0);

    for (const auto& granted_role : granted_roles) {
      const auto& role_info = roles_map_[granted_role];
      auto l = role_info->LockForRead();
      const auto& pb = l->data().pb;

      Permissions all_roles_permissions_bitset(role_permissions->all_roles_permissions());
      Permissions all_keyspaces_permissions_bitset(role_permissions->all_keyspaces_permissions());

      for (const auto& resource : pb.resources()) {
        Permissions resource_permissions_bitset;

        for (const auto& permission : resource.permissions()) {
          if (resource.canonical_resource() == kRolesRoleResource) {
            all_roles_permissions_bitset.set(permission);
          } else if (resource.canonical_resource() == kRolesDataResource) {
            all_keyspaces_permissions_bitset.set(permission);
          } else {
            resource_permissions_bitset.set(permission);
          }
        }

        if (resource.canonical_resource() != kRolesDataResource &&
            resource.canonical_resource() != kRolesRoleResource) {
          auto* resource_permissions = role_permissions->add_resource_permissions();
          resource_permissions->set_canonical_resource(resource.canonical_resource());
          resource_permissions->set_permissions(resource_permissions_bitset.to_ullong());
          recursive_granted_permissions_[role_name][resource.canonical_resource()] =
              resource_permissions_bitset.to_ullong();
        }
      }

      role_permissions->set_all_keyspaces_permissions(all_keyspaces_permissions_bitset.to_ullong());
      VLOG(2) << "Setting all_keyspaces_permissions to "
              << role_permissions->all_keyspaces_permissions()
              << " for role " << role_name;

      role_permissions->set_all_roles_permissions(all_roles_permissions_bitset.to_ullong());
      VLOG(2) << "Setting all_roles_permissions to "
              << role_permissions->all_roles_permissions()
              << " for role " << role_name;

      // TODO: since this gets checked first when enforcing permissions, there is no point in
      // populating the rest of the permissions. Furthermore, we should remove any specific
      // permissions in the map.
      // In other words, if all-roles_all_keyspaces_permissions is equal to superuser_permissions,
      // it should be the only field sent to the clients for that specific role.
      if (pb.is_superuser()) {
        Permissions superuser_bitset;
        // Set all the bits to 1.
        superuser_bitset.set();

        role_permissions->set_all_keyspaces_permissions(superuser_bitset.to_ullong());
        role_permissions->set_all_roles_permissions(superuser_bitset.to_ullong());
      }
    }
  }

  auto config = CHECK_NOTNULL(security_config_.get())->LockForRead();
  response->set_version(config->data().pb.security_config().roles_version());
  permissions_cache_ = std::move(response);
}

// Get all the permissions granted to resources.
Status PermissionsManager::GetPermissions(
    const GetPermissionsRequestPB* req,
    GetPermissionsResponsePB* resp,
    rpc::RpcContext* rpc) {
  std::shared_ptr<GetPermissionsResponsePB> permissions_cache;
  {
    std::lock_guard<decltype(catalog_manager_->lock_)> l_big(catalog_manager_->lock_);
    if (!permissions_cache_) {
      BuildRecursiveRolesUnlocked();
      if (!permissions_cache_) {
        DFATAL_OR_RETURN_NOT_OK(STATUS(IllegalState, "Unable to build permissions cache"));
      }
    }
    // Create another reference so that the cache doesn't go away while we are using it.
    permissions_cache = permissions_cache_;
  }

  boost::optional<uint64_t> request_version;
  if (req->has_if_version_greater_than()) {
    request_version = req->if_version_greater_than();
  }

  if (request_version && permissions_cache->version() == *request_version) {
    resp->set_version(permissions_cache->version());
    return Status::OK();
  } else if (request_version && permissions_cache->version() < *request_version) {
    LOG(WARNING) << "GetPermissionsRequestPB version is greater than master's version";
    Status s = STATUS_SUBSTITUTE(IllegalState,
        "GetPermissionsRequestPB version $0 is greater than master's version $1. "
        "Should call GetPermissions again",  *request_version,
        permissions_cache->version());
    return SetupError(resp->mutable_error(), MasterErrorPB::CONFIG_VERSION_MISMATCH, s);
  }

  // permisions_cache_->version() > req->if_version_greater_than() or
  // req->if_version_greather_than() is not set.
  resp->CopyFrom(*permissions_cache);
  return Status::OK();
}

bool PermissionsManager::IsMemberOf(const RoleName& granted_role, const RoleName& role) {
  const auto& iter = recursive_granted_roles_.find(role);
  if (iter == recursive_granted_roles_.end()) {
    // No roles have been granted to role.
    return false;
  }

  const auto& granted_roles = iter->second;
  if (granted_roles.find(granted_role) == granted_roles.end()) {
    // granted_role has not been granted directly or through inheritance to role.
    return false;
  }

  return true;
}

Status PermissionsManager::GrantRevokePermission(
    const GrantRevokePermissionRequestPB* req,
    GrantRevokePermissionResponsePB* resp,
    rpc::RpcContext* rpc) {
  LOG(INFO) << (req->revoke() ? "Revoke" : "Grant") << " permission "
            << RequestorString(rpc) << ": " << req->ShortDebugString();
  RETURN_NOT_OK(catalog_manager_->CheckOnline());

  std::lock_guard<decltype(catalog_manager_->lock_)> l_big(catalog_manager_->lock_);
  TRACE("Acquired catalog manager lock");
  Status s;
  scoped_refptr<NamespaceInfo> ns;
  scoped_refptr<TableInfo> table;

  // Checking if resources exist.
  if (req->resource_type() == ResourceType::TABLE ||
      req->resource_type() == ResourceType::KEYSPACE) {
    // We can't match Apache Cassandra's error because when a namespace is not provided, the error
    // is detected by the semantic analysis in PTQualifiedName::AnalyzeName.
    DCHECK(req->has_namespace_());
    const auto& namespace_info = req->namespace_();
    s = catalog_manager_->FindNamespaceUnlocked(namespace_info, &ns);

    if (req->resource_type() == ResourceType::KEYSPACE) {
      if (ns == nullptr) {
        // Matches Apache Cassandra's error.
        s = STATUS_SUBSTITUTE(
            NotFound, "Resource <keyspace $0> doesn't exist", namespace_info.name());
        return SetupError(resp->mutable_error(), MasterErrorPB::NAMESPACE_NOT_FOUND, s);
      }
    } else {
      if (ns) {
        table = FindPtrOrNull(catalog_manager_->table_names_map_, {ns->id(), req->resource_name()});
      }
      if (table == nullptr) {
        // Matches Apache Cassandra's error.
        s = STATUS_SUBSTITUTE(
            NotFound, "Resource <object '$0.$1'> doesn't exist",
            namespace_info.name(), req->resource_name());
        return SetupError(resp->mutable_error(), MasterErrorPB::OBJECT_NOT_FOUND, s);
      }
    }
  }

  if (req->resource_type() == ResourceType::ROLE) {
    scoped_refptr<RoleInfo> role;
    role = FindPtrOrNull(roles_map_, req->resource_name());
    if (role == nullptr) {
      s = STATUS_SUBSTITUTE(NotFound, "Resource <role $0> does not exist", req->role_name());
      return SetupError(resp->mutable_error(), MasterErrorPB::ROLE_NOT_FOUND, s);
    }
  }

  scoped_refptr<RoleInfo> rp;
  rp = FindPtrOrNull(roles_map_, req->role_name());
  if (rp == nullptr) {
    s = STATUS_SUBSTITUTE(InvalidArgument, "Role $0 doesn't exist", req->role_name());
    return SetupError(resp->mutable_error(), MasterErrorPB::ROLE_NOT_FOUND, s);
  }

  // Increment roles version.
  RETURN_NOT_OK(IncrementRolesVersionUnlocked());

  SysRoleEntryPB* metadata;
  {
    ScopedMutation<RoleInfo> role_info_mutation(rp.get());
    metadata = &rp->mutable_metadata()->mutable_dirty()->pb;

    ResourcePermissionsPB* current_resource = nullptr;
    auto current_resource_iter = metadata->mutable_resources()->end();
    for (current_resource_iter = metadata->mutable_resources()->begin();
         current_resource_iter != metadata->mutable_resources()->end(); current_resource_iter++) {
      if (current_resource_iter->canonical_resource() == req->canonical_resource()) {
        break;
      }
    }

    if (current_resource_iter != metadata->mutable_resources()->end()) {
      current_resource = &(*current_resource_iter);
    }

    if (current_resource == nullptr) {
      if (req->revoke()) {
        return Status::OK();
      }

      current_resource = metadata->add_resources();
      current_resource_iter = std::prev(metadata->mutable_resources()->end());

      current_resource->set_canonical_resource(req->canonical_resource());
      current_resource->set_resource_type(req->resource_type());
      if (req->has_resource_name()) {
        current_resource->set_resource_name(req->resource_name());
      }
      if (req->has_namespace_()) {
        current_resource->set_namespace_name(req->namespace_().name());
      }
    }

    if (req->permission() != PermissionType::ALL_PERMISSION) {
      auto permission_iter = current_resource->permissions().end();
      for (permission_iter = current_resource->permissions().begin();
           permission_iter != current_resource->permissions().end(); permission_iter++) {
        if (*permission_iter == req->permission()) {
          break;
        }
      }

      // Resource doesn't have the permission, and we got a GRANT request.
      if (permission_iter == current_resource->permissions().end() && !req->revoke()) {
        // Verify that the permission is supported by the resource.
        if (!valid_permission_for_resource(req->permission(), req->resource_type())) {
          s = STATUS_SUBSTITUTE(InvalidArgument, "Invalid permission $0 for resource type $1",
              req->permission(), ResourceType_Name(req->resource_type()));
          // This should never happen because invalid permissions get rejected in the analysis part.
          // So crash the process if in debug mode.
          DFATAL_OR_RETURN_NOT_OK(s);
          return SetupError(resp->mutable_error(), MasterErrorPB::INVALID_REQUEST, s);
        }
        current_resource->add_permissions(req->permission());
      } else if (permission_iter != current_resource->permissions().end() && req->revoke()) {
        current_resource->mutable_permissions()->erase(permission_iter);
      }
    } else {
      // ALL permissions.
      // TODO (Bristy) : Add different permissions for different resources based on role names.
      // For REVOKE ALL we clear all the permissions and do nothing else.
      current_resource->clear_permissions();

      if (!req->revoke()) {
        for (const auto& permission : all_permissions_for_resource(req->resource_type())) {
          current_resource->add_permissions(permission);
        }
      }
    }

    // If this resource doesn't have any more permissions, remove it.
    if (current_resource->permissions().empty()) {
      metadata->mutable_resources()->erase(current_resource_iter);
    }

    s = catalog_manager_->sys_catalog_->UpdateItem(rp.get(),
        catalog_manager_->leader_ready_term());
    if (!s.ok()) {
      s = s.CloneAndPrepend(Substitute(
          "An error occurred while updating permissions in sys-catalog: $0", s.ToString()));
      LOG(WARNING) << s.ToString();
      return CheckIfNoLongerLeaderAndSetupError(s, resp);
    }
    TRACE("Wrote Permission to sys-catalog");

    role_info_mutation.Commit();
  }
  LOG(INFO) << "Modified Permission for role " << rp->id();
  BuildResourcePermissionsUnlocked();
  return Status::OK();
}

void PermissionsManager::GetAllRoles(std::vector<scoped_refptr<RoleInfo>>* roles) {
  roles->clear();
  SharedLock<decltype(catalog_manager_->lock_)> l(catalog_manager_->lock_);
  for (const RoleInfoMap::value_type& e : roles_map_) {
    roles->push_back(e.second);
  }
}

vector<string> PermissionsManager::DirectMemberOf(const RoleName& role) {
  vector<string> roles;
  for (const auto& e : roles_map_) {
    auto l = e.second->LockForRead();
    const auto& pb = l->data().pb;
    for (const auto& member_of : pb.member_of()) {
      if (member_of == role) {
        roles.push_back(pb.role());
        // No need to keep checking the rest of the members.
        break;
      }
    }
  }
  return roles;
}

void PermissionsManager::BuildRecursiveRoles() {
  TRACE("Acquired catalog manager lock");
  std::lock_guard<decltype(catalog_manager_->lock_)> l_big(catalog_manager_->lock_);
  BuildRecursiveRolesUnlocked();
}

void PermissionsManager::TraverseRole(
    const string& role_name, unordered_set<RoleName>* granted_roles) {
  auto iter = recursive_granted_roles_.find(role_name);
  // This node has already been visited. So just add all the granted (directly or through
  // inheritance) roles to granted_roles.
  if (iter != recursive_granted_roles_.end()) {
    if (granted_roles) {
      const auto& set = iter->second;
      granted_roles->insert(set.begin(), set.end());
    }
    return;
  }

  const auto& role_info = roles_map_[role_name];
  auto l = role_info->LockForRead();
  const auto& pb = l->data().pb;
  if (pb.member_of().size() == 0) {
    recursive_granted_roles_.insert({role_name, {}});
  } else {
    for (const auto& direct_granted_role : pb.member_of()) {
      recursive_granted_roles_[role_name].insert(direct_granted_role);
      TraverseRole(direct_granted_role, &recursive_granted_roles_[role_name]);
    }
    if (granted_roles) {
      const auto& set = recursive_granted_roles_[role_name];
      granted_roles->insert(set.begin(), set.end());
    }
  }
}

void PermissionsManager::AddRoleUnlocked(
    const RoleName& role_name,
    scoped_refptr<RoleInfo> role_info) {
  roles_map_[role_name] = std::move(role_info);
}

bool PermissionsManager::DoesRoleExistUnlocked(const RoleName& role_name) {
  return roles_map_.count(role_name);
}

void PermissionsManager::ClearRolesUnlocked() {
  roles_map_.clear();
}

Status PermissionsManager::PrepareDefaultSecurityConfigUnlocked(int64_t term) {
  // Set up default security config if not already present.
  if (!security_config_) {
    SysSecurityConfigEntryPB security_config;
    security_config.set_roles_version(0);

    // Create in memory object.
    security_config_ = new SysConfigInfo(kSecurityConfigType);

    // Prepare write.
    auto l = security_config_->LockForWrite();
    *l->mutable_data()->pb.mutable_security_config() = std::move(security_config);

    // Write to sys_catalog and in memory.
    RETURN_NOT_OK(catalog_manager_->sys_catalog_->AddItem(security_config_.get(), term));
    l->Commit();
  }
  return Status::OK();
}

void PermissionsManager::SetSecurityConfigOnLoadUnlocked(SysConfigInfo* security_config) {
  LOG_IF(WARNING, security_config_ != nullptr)
      << "Multiple security configs found when loading sys catalog";
  security_config_ = security_config;
}

}  // namespace master
}  // namespace yb
