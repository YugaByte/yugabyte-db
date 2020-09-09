---
title: Grant YSQL privileges in YugabyteDB
headerTitle: Grant privileges
linkTitle: Grant privileges
description: Grant YSQL privileges in YugabyteDB
menu:
  latest:
    name: Grant privileges
    identifier: ysql-grant-permissions
    parent: authorization
    weight: 735  
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/secure/authorization/ysql-grant-permissions" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>
  <li >
    <a href="/latest/secure/authorization/ycql-grant-permissions" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

In this tutorial, you will run through a scenario. Assume a company has an engineering organization, with three sub-teams - developers, qa and DB admins. We are going to create a role for each of these entities.

Here is what you want to achieve from a role-based access control (RBAC) perspective.

+ All members of engineering should be able to read data from any database and table.
+ Both developers and qa should be able to modify data in existing tables in the database `dev_database`.
+ QA should be able to alter the `integration_tests` table in the database `dev_database`.
+ DB admins should be able to perform all operations on any database.

## 1. Create role hierarchy

Connect to the cluster using a superuser role. Read more about [enabling authentication and connecting using a superuser role](../../authentication/ysql-authentication/) in YugabyteDB clusters for YSQL. For this tutorial, you are using the default `yugabyte` user and connect to the cluster using `ysqlsh` as follows:

```sh
$ ysqlsh
```

Create a database `dev_database`.

```postgresql
yugabyte=# CREATE database dev_database;
```

Switch to the `dev_database`.

```
yugabyte=# \c dev_database
```

Create the `integration_tests` table:

```postgresql
dev_database=# CREATE TABLE integration_tests (
                 id UUID PRIMARY KEY,
                 time TIMESTAMP,
                 result BOOLEAN,
                 details JSONB
                 );
```

Next, create roles `engineering`, `developer`, `qa`, and `db_admin`.

```postgresql
dev_database=# CREATE ROLE engineering;
                 CREATE ROLE developer;
                 CREATE ROLE qa;
                 CREATE ROLE db_admin;
```

Grant the `engineering` role to `developer`, `qa`, and `db_admin` roles since they are all a part of the engineering organization.

```postgresql
dev_database=# GRANT engineering TO developer;
                 GRANT engineering TO qa;
                 GRANT engineering TO db_admin;
```

List all the roles amd their memberships.

```
yugabyte=# \du
```

You should see the following output:

```
                                      List of roles
  Role name  |                         Attributes                         |   Member of
-------------+------------------------------------------------------------+---------------
 db_admin    | Cannot login                                               | {engineering}
 developer   | Cannot login                                               | {engineering}
 engineering | Cannot login                                               | {}
 postgres    | Superuser, Create role, Create DB, Replication, Bypass RLS | {}
 qa          | Cannot login                                               | {engineering}
 yugabyte    | Superuser, Create role, Create DB, Replication, Bypass RLS | {}
```

## 2. List privileges for roles

You can list all privileges granted to the various roles with the following command:

```
yugabyte=# \du
```

You should see something like the following output.

```
                                      List of roles
  Role name  |                         Attributes                         |   Member of
-------------+------------------------------------------------------------+---------------
 db_admin    | Cannot login                                               | {engineering}
 developer   | Cannot login                                               | {engineering}
 engineering | Cannot login                                               | {}
 postgres    | Superuser, Create role, Create DB, Replication, Bypass RLS | {}
 qa          | Cannot login                                               | {engineering}
 yugabyte    | Superuser, Create role, Create DB, Replication, Bypass RLS | {}
```

The above shows the various role attributes the `yugabyte` role has. Since `yugabyte` is a superuser, it has all privileges on all databases, including `ALTER`, `Create role` and `DROP` on the roles you created (`engineering`, `developer`, `qa` and `db_admin`).

## 3. Grant privileges to roles

In this section, you will grant privileges to achieve the following as mentioned in the beginning of this tutorial:

+ All members of engineering should be able to read (`SELECT`) data from any database and table.
+ Both developers and qa should be able to modify (`INSERT`, `UPDATE`, and `DELETE`) data in existing tables in the database `dev_database`.
+ Developers should be able to create, alter and drop tables in the database `dev_database`.
+ DB admins should be able to perform all operations on any database.

### Grant read access

All members of engineering should be able to read data from any database and table. Use the `GRANT` statement to grant `SELECT` (or read) access on the existing table (`integration_tests`) to the `engineering` role. This can be done as follows:

```postgresql
dev_database=# GRANT SELECT ON ALL TABLE integration_tests to engineering;
dev_database=# GRANT USAGE ON SCHEMA public TO engineering;
```

You can now verify that the `engineering` role has `SELECT` privilege as follows:

```
dev_database=# \z
```

The output should look similar to below, where you see that the `engineering` role has `SELECT` privilege on the `data` resource.

```
 Schema |       Name        | Type  |     Access privileges     | Column privileges | Policies
--------+-------------------+-------+---------------------------+-------------------+----------
 public | integration_tests | table | yugabyte=arwdDxt/yugabyte+|                   |
        |                   |       | engineering=r/yugabyte   +|                   |
```

The access privileges "arwdDxt" include all privileges for the user `yugabyte` (superuser), while the role `engineering` has only "r" (read) privileges. For details on the `GRANT` statement and access privileges, see [GRANT](../../../api/ysql/commands/dcl_grant).

Granting the role `engineering` to any other role will cause all those roles to inherit the specified privileges. Thus, `developer`, `qa` and `db_admin` will all inherit the `SELECT` and `USAGE` privileges, giving them read-access.

### Grant data modify access

Both `developers` and `qa` should be able to modify data existing tables in the database `dev_database`. They should be able to execute statements such as `INSERT`, `UPDATE`, `DELETE` or `TRUNCATE` in order to modify data on existing tables. This can be done as follows:

```postgresql
dev_database=# GRANT INSERT, UPDATE, DELETE, TRUNCATE ON table integration_tests TO developer;
dev_database=# GRANT INSERT, UPDATE, DELETE, TRUNCATE ON table integration_tests TO qa;
```

You can verify that the `developer` and `qa` roles have the appropriate privileges by running the ysqlsh `\z` command again.

```
dev_database=# \z
```

Now `developer` and `qa` roles have the access privileges `awdD` (append/insert, write/update, delete, and truncate) for the table `integration_tests`.

```
                                       Access privileges
 Schema |       Name        | Type  |     Access privileges     | Column privileges | Policies
--------+-------------------+-------+---------------------------+-------------------+----------
 public | integration_tests | table | yugabyte=arwdDxt/yugabyte+|                   |
        |                   |       | engineering=r/yugabyte   +|                   |
        |                   |       | developer=awdD/yugabyte  +|                   |
        |                   |       | qa=awdD/yugabyte          |                   |
```

### Grant alter table access

QA (`qa`) should be able to alter the table `integration_tests` in the database `dev_database`. This can be done as follows.

```postgresql
yugabyte=# ALTER TABLE integration_tests OWNER TO qa;
```

Once again, run the following command to verify the privileges.

```postgresql
yugabyte=# SELECT * FROM system_auth.role_privileges;
```

We should see that owner has changed from `yugabyte` to `qa` and `qa` has all access privileges (`arwdDxt`) on the table `integration_tests`.

```
                                   Access privileges
 Schema |       Name        | Type  | Access privileges | Column privileges | Policies
--------+-------------------+-------+-------------------+-------------------+----------
 public | integration_tests | table | qa=arwdDxt/qa    +|                   |
        |                   |       | steve=arw/qa     +|                   |
        |                   |       | engineering=r/qa +|                   |
        |                   |       | test=r/qa        +|                   |
        |                   |       | eng=r/qa         +|                   |
        |                   |       | developer=awdD/qa |                   |
```

### Grant all privileges

DB admins should be able to perform all operations on any database. There are two ways to achieve this:

1. The DB admins can be granted the superuser privilege. Read more about [granting the superuser privilege to roles](../../authentication/ysql-authentication). Note that doing this will give the DB admin all the privileges over all the roles as well.

2. Grant `ALL` privileges to the `db_admin` role. This can be achieved as follows.

```postgresql
dev_database=# ALTER USER db_admin WITH SUPERUSER;
```

Run the following command to verify the privileges:

```postgresql
dev_database=# \du
```

We should see the following, which grants the `Superuser` privileges on the  to the role `db_admin`.

```
                                      List of roles
  Role name  |                         Attributes                         |   Member of
-------------+------------------------------------------------------------+---------------
 db_admin    | Superuser                                                  | {engineering}
 developer   | Cannot login                                               | {engineering}
 eng         | Cannot login                                               | {}
 engineering | Cannot login                                               | {}
 postgres    | Superuser, Create role, Create DB, Replication, Bypass RLS | {}
 qa          | Cannot login                                               | {engineering}
 yugabyte    | Superuser, Create role, Create DB, Replication, Bypass RLS | {}
```

## 4. Revoke privileges from roles

Let us say you want to revoke the `Superuser` privilege from the DB admins so that they can no longer change privileges for other roles. This can be done as follows.

```postgresql
yugabyte=# ALTER USER db_admin WITH NOSUPERUSER;
```

Run the following command to verify the privileges.

```postgresql
yugabyte=# \du
```

We should see the following output.

```
                                      List of roles
  Role name  |                         Attributes                         |   Member of
-------------+------------------------------------------------------------+---------------
 db_admin    |                                                            | {engineering}
 developer   | Cannot login                                               | {engineering}
 eng         | Cannot login                                               | {}
 engineering | Cannot login                                               | {}
 postgres    | Superuser, Create role, Create DB, Replication, Bypass RLS | {}
 qa          | Cannot login                                               | {engineering}
 steve       |                                                            | {}
 test        |                                                            | {}
 yugabyte    | Superuser, Create role, Create DB, Replication, Bypass RLS | {}
```

The `Superuser` privilege is no longer granted to the `db_admin` role.
