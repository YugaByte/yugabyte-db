---
title: Authorization platform
headerTitle: Authorization platform
linkTitle: Authorization platform
description: Use the role-based access control (RBAC) model in the platform to manage users and roles.
menu:
  stable:
    parent: security
    identifier: authorization-platform
    weight: 27
isTocNested: true
showAsideToc: true
---

The role-based access control (RBAC) model in the platform is a collection of privileges on resources given to roles. Thus, the entire RBAC model is built around roles, resources, and privileges. It is essential to understand these concepts in order to understand the RBAC model.

#### Users

A platform user can interact with a platform instance using the UI or through a REST API.

#### Roles

A role is a set of predefined permissions within the Yugabyte Platform. These are the available roles:

* **Super Admin:** The [admin user](../../configure-yugabyte-platform/create-admin-user/) is the first user that is created during the tenant registration. This role has the highest level of privilege that allows all read and write actions on all platform resources. There can be only one Super Admin in a tenant:
  * Manage all resources - Universe, Nodes, Backups, Restore, Cloud Provider
  * Manage user access control - Create and Manage users
* **Admin:** This admin user has the same privilege as a Super Admin.
* **Backup Admin:** Enables a user to access the backup related tasks:
  * Manage database backups and restore operations.
  * Create new backups.
  * Delete any existing backup or backup schedule.
  * Edit existing backups.
  * Read-only permissions for all other resources in the platform.
* **Read-only:** Enables a user to have read-only viewer permission to the UI and API.

#### Create and manage users

As a Super Admin or Admin, you can invite new users and manage existing users for your tenant on the Users Administration page, which you can reach by selecting the drop-down list (the profile icon) in the top-right corner of the Yugabyte Platform console and then click **Profile**.

To invite new users to your tenant:

1. Click on the **Users** tab.
2. Click **Add user**.
3. Enter the email addresses, password, and role of who you want to invite.
4. Click **Submit**.

![Add User](/images/yp/authorization-platform/add-user.png)

Now you can share the new credentials with the new users to log in.

#### To edit existing user roles

1. Click **Actions** for a username to edit user role or delete user.

![alt_text](/images/yp/authorization-platform/actions.png)

2. You can choose a new role from the **Role** drop-down list.

![Role drop-down list](/images/yp/authorization-platform/role-drop-down.png)
