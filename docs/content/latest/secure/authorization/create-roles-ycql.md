---
title: Create YCQL roles in YugabyteDB
headerTitle: Create roles
linkTitle: Create roles
description: Create YCQL roles in YugabyteDB
headcontent: Creating roles
image: /images/section_icons/secure/create-roles.png
menu:
  latest:
    identifier: create-roles-ycql
    parent: authorization
    weight: 717
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/secure/authorization/create-roles" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="/latest/secure/authorization/create-roles-ycql" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

## 1. Create roles

Create a role with a password. You can do this with the [CREATE ROLE](../../../api/ycql/ddl_create_role/) command.

As an example, let us create a role `engineering` for an engineering team in an organization. Note that you add the `IF NOT EXISTS` clause in case the role already exists.

```sql
cassandra@ycqlsh> CREATE ROLE IF NOT EXISTS engineering;
```

Roles that have `LOGIN` permissions are users. As an example, you can create a user `john` as follows:

```sql
cassandra@ycqlsh> CREATE ROLE IF NOT EXISTS john WITH PASSWORD = 'PasswdForJohn' AND LOGIN = true;
```

Read about [how to create users in YugabyteDB](../../authentication/) in the authentication section.

## 2. Grant roles

You can grant a role to another role (which can be a user), or revoke a role that has already been granted. Executing the `GRANT` and the `REVOKE` operations requires the `AUTHORIZE` permission on the role being granted or revoked.

As an example, you can grant the `engineering` role you created above to the user `john` as follows:

```sql
cassandra@ycqlsh> GRANT engineering TO john;
```

Read more about [granting roles](../../../api/ycql/ddl_grant_role/).

## 3. Create a hierarchy of roles, if needed

In YCQL, you can create a hierarchy of roles. The permissions of any role in the hierarchy flows downward.

As an example, let us say that in the above example, you want to create a `developer` role that inherits all the permissions from the `engineering` role. You can achieve this as follows.

First, create the `developer` role.

```sql
cassandra@ycqlsh> CREATE ROLE IF NOT EXISTS developer;
```

Next, `GRANT` the `engineering` role to the `developer` role.

```sql
cassandra@ycqlsh> GRANT engineering TO developer;
```

## 4. List roles

You can list all the roles by running the following command:

```sql
cassandra@ycqlsh> SELECT role, can_login, is_superuser, member_of FROM system_auth.roles;
```

You should see the following output:

```
 role        | can_login | is_superuser | member_of
-------------+-----------+--------------+-----------------
        john |      True |        False | ['engineering']
   developer |     False |        False | ['engineering']
 engineering |     False |        False |                []
   cassandra |      True |         True |                []

(4 rows)
```

In the table above, note the following:

* The `cassandra` role is the built-in superuser.
* The role `john` can login, and hence is a user. Note that `john` is not a superuser.
* The roles `engineering` and `developer` cannot login.
* Both `john` and `developer` inherit the role `engineering`.

## 5. Revoke roles

Roles can be revoked using the [REVOKE ROLE](../../../api/ycql/ddl_revoke_role/) command.

In the above example, you can revoke the `engineering` role from the user `john` as follows:

```sql
cassandra@ycqlsh> REVOKE engineering FROM john;
```

Listing all the roles now shows that `john` no longer inherits from the `engineering` role:

```sql
cassandra@ycqlsh> SELECT role, can_login, is_superuser, member_of FROM system_auth.roles;
```

```
 role        | can_login | is_superuser | member_of
-------------+-----------+--------------+-----------------
        john |      True |        False |                []
   developer |     False |        False | ['engineering']
 engineering |     False |        False |                []
   cassandra |      True |         True |                []

(4 rows)
```

## 6. Drop roles

Roles can be dropped with the [DROP ROLE](../../../api/ycql/ddl_drop_role/) command.

In the above example, you can drop the `developer` role with the following command:

```sql
cassandra@ycqlsh> DROP ROLE IF EXISTS developer;
```

The `developer` role would no longer be present upon listing all the roles:

```sql
cassandra@ycqlsh> SELECT role, can_login, is_superuser, member_of FROM system_auth.roles;
```

```
 role        | can_login | is_superuser | member_of
-------------+-----------+--------------+-----------
        john |      True |        False |          []
 engineering |     False |        False |          []
   cassandra |      True |         True |          []

(3 rows)
```
