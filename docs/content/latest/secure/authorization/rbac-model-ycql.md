---
title: Role-based access control (RBAC) model in YCQL
linkTitle: Overview
headerTitle: Role-based access overview
description: Overview of the role-based access control (RBAC) model in YCQL.
headcontent: How role-based access control works
image: /images/section_icons/secure/rbac-model.png
menu:
  latest:
    identifier: rbac-model-ycql
    parent: authorization
    weight: 716
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/secure/authorization/rbac-model" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="/latest/secure/authorization/rbac-model-ycql" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

The role-based access control (RBAC) model in YCQL is a collection of permissions on resources given to roles. Thus, the entire RBAC model is built around roles, resources, and permissions. It is essential to understand these concepts in order to understand the RBAC model.

## Roles

Roles in YCQL can represent individual users or a group of users. They encapsulate a set of permissions that can be assigned to other roles (or users). Roles are essential to implementing and administering access control on a YugabyteDB cluster. Below are some important points about roles:

* Roles which have login permission are users. Hence, all users are roles but all roles are not users.

* Roles can be granted to other roles, making it possible to organize roles into a hierarchy.

* Roles inherit the permissions of all other roles granted to them.

## Resources

YCQL defines a number of specific resources, that represent underlying database objects. A resource can denote one object or a collection of objects. YCQL resources are hierarchical as described below:

* Keyspaces and tables follow the hierarchy: `ALL KEYSPACES` > `KEYSPACE` > `TABLE`
* ROLES are hierarchical (they can be assigned to other roles). They follow the hierarchy: `ALL ROLES` > `ROLE #1` > `ROLE #2` ...

The table below lists out the various resources.

Resource        | Description |
----------------|-------------|
`KEYSPACE`      | Denotes one keyspace. Typically includes all the tables and indexes defined in that keyspace. |
`TABLE`         | Denotes one table. Includes all the indexes defined on that table. |
`ROLE`          | Denotes one role. |
`ALL KEYSPACES` | Collection of all keyspaces in the database. |
`ALL ROLES`     | Collection of all roles in the database. |

## Permissions

Permissions are necessary to execute operations on database objects. Permissions can be granted at any level of the database hierarchy and are inherited downwards. The set of permissions include:

Permission  | Objects                      | Operations                          |
------------|------------------------------|-------------------------------------|
`ALTER`     | keyspace, table, role        | ALTER                               |
`AUTHORIZE` | keyspace, table, role        | GRANT PERMISSION, REVOKE PERMISSION |
`CREATE`    | keyspace, table, role, index | CREATE                              |
`DROP`      | keyspace, table, role, index | DROP                                |
`MODIFY`    | keyspace, table              | INSERT, UPDATE, DELETE, TRUNCATE    |
`SELECT`    | keyspace, table              | SELECT                              |
`DESCRIBE` (not implemented)  | role       | LIST ROLES                          |

{{< note title="Note" >}}

The `ALTER` permission on the base table is required in order to CREATE or DROP indexes on it.

{{< /note >}}

Read more about [permissions in YCQL](../../../api/ycql/ddl_grant_permission/#permissions).
