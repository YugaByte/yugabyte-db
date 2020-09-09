---
title: YSQL authentication
headerTitle: YSQL authentication
linkTitle: YSQL authentication
description: Identify that your YSQL users are who they say they are.
headcontent: Identify that your YSQL users are who they say they are.
image: /images/section_icons/secure/authentication.png
block_indexing: true
menu:
  stable:
    name: Authentication
    identifier: ysql-authentication
    parent: authentication
    weight: 721
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/stable/secure/authentication/ysql-authentication" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>
  <li >
    <a href="/stable/secure/authentication/ycql-authentication" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
  <li>
    <a href="/stable/secure/authentication/yedis-authentication" class="nav-link">
      <i class="icon-redis" aria-hidden="true"></i>
      YEDIS
    </a>
  </li>
</ul>

YSQL authentication, the process of identifying that YSQL users are who they say they are, is based on roles. Users, groups, and roles within YugabyteDB are created using roles. Typically, a role that has login privileges is known as a *user*, while a *group* is a role that can have multiple users as members. 

Users, roles, and groups allow administrators to verify whether a particular user or role is authorized to create, access, change, or remove databases or manage users and roles. [Authorization](../../authorization/) is the process of managing access controls based on roles. For YSQL, enabling authentication automatically enables authorization and the [role-based access control (RBAC) model](../../authorization/rbac-model/), to determine the access privileges. Authentication verifies the identity of a user while authorization determines the verified user’s database access privileges.

Users and roles can be created with superuser, non-superuser, and login privileges, and the roles that users have are used to determine what access privileges are available. Administrators can create users and roles using the [`CREATE ROLE`](../../../api/ysql/commands/dcl_create_role/) statement (or its alias, [`CREATE USER`](../../../api/ysql/commands/dcl_create_user/)). After users and roles have been created, [`ALTER ROLE`](../../../api/ysql/commands/dcl_alter_role/) and [`DROP ROLE`](../../../api/ysql/commands/dcl_drop_role/) statements are used to change or remove users and roles.

YSQL authorization is the process of access control created by granting or revoking privileges to YSQL users and roles, see [Authorization](../../authorization). Privileges are managed using [`GRANT`](../../../api/ysql/commands/dcl_grant/), [`REVOKE`](../../../api/ysql/commands/dcl_revoke/), [`CREATE ROLE`](../../../api/ysql/commands/dcl_create_role/), [`ALTER ROLE`](../../../api/ysql/commands/dcl_alter_role/), and [`DROP ROLE`](../../../api/ysql/commands/dcl_drop_role/).

## Specify default user password

When you start a YugabyteDB cluster, the YB-Master and YB-TServer services are launched using the default user, named `yugabyte`, and then this user is connected to the default database, also named `yugabyte`. When YSQL authentication is enabled, all users (including `yugabyte`) require a password to log into a YugabyteDB database. Before you start YugabyteDB with YSQL authentication enabled, you need to make sure that the `yugabyte` user has a password. 

Starting in YugabyteDB 2.0.1, the default `yugabyte` user has a default password of `yugabyte` that lets this user sign into YugabyteDB when YSQL authentication is enabled. If you are using YugabyteDB 2.0.1 or later, you can skip the steps here to create a password and jump to the next section on enabling YSQL authentication.

If you are using YugabyteDB 2.0 (and **not** 2.0.1 or later) and have not assigned a password to the `yugabyte` user yet, follow these steps to quickly add a password:

1. With your YugabyteDB cluster up and running, open `ysqlsh`.
2. Run the following `ALTER ROLE` statement, specifying a password (`yugabyte` or a password of your choice).

    ```postgresql
    yugabyte=# ALTER ROLE yugabyte with password 'yugabyte';
    ```

Assuming that you've successfully added a password for the `yugabyte` user, you can continue to the next section and learn how to start, or restart, your YugabyteDB cluster with YSQL authentication enabled.

## Enable YSQL authentication

### Start local clusters

To enable YSQL authentication in your local YugabyteDB clusters, you can add the add the [--ysql_enable_auth flag](../../../reference/configuration/yb-tserver/#ysql-flags) via the [--tserver_flags flag](../../../admin/yb-ctl/#create-a-cluster-with-custom-flags) with the `yb-ctl create`, `yb-ctl start`, and `yb-ctl restart`.

When you create a local cluster, you can run the `yb-ctl create` command like this to enable YSQL authentication in the newly-created cluster.

```sh
./bin/yb-ctl create --tserver_flags "ysql_enable_auth=true"
```

After your local cluster has been created, you can enable YSQL authentication when you start your cluster with a `yb-ctl start` command like this:

```sh
./bin/yb-ctl start --tserver_flags "ysql_enable_auth=true"
```

To restart your cluster, you can run the `yb-ctl restart` command with  the `--tserver_flags` flag to restart your cluster, like this:

```sh
./bin/yb-ctl restart --tserver_flags "ysql_enable_auth=true"
```

### Start YB-TServer services

To enable YSQL authentication in deployable YugabyteDB clusters, you need to start your `yb-tserver` services using the [`--ysql_enable_auth` flag](../../../reference/configuration/yb-tserver#ysql-enable-auth). Your command should look similar to this command:

```sh
./bin/yb-tserver \
  --tserver_master_addrs <master addresses> \
  --fs_data_dirs <data directories> \
  --ysql_enable_auth=true \
  >& /home/centos/disk1/yb-tserver.out &
```

You can also enable YSQL authentication by adding the `--ysql_enable_auth=true` to the YB-TServer configuration file (`tserver.conf`). For more information, see [Start YB-TServers](../../../deploy/manual-deployment/start-tservers/).

## Open the YSQL shell (ysqlsh)

A YugabyteDB cluster with authentication enabled starts with the default admin user of `yugabyte` and the default database of `yugabyte`. You can connect to the cluster and use the YSQL shell by running the following `ysqlsh` command from the YugabyteDB home directory:

```sh
$ ./bin/ysqlsh -U yugabyte
```

You will be prompted to enter the password. Upon successful login to the YSQL shell, you will see the following:

```
ysqlsh (11.2-YB-2.0.0.0-b16)
Type "help" for help.

yugabyte=#
```

## Common user authentication tasks

Here are some common authentication-related tasks. For authorization-related tasks, see [Authorization](../../authorization).

### Creating users

To add a new user, run the [`CREATE ROLE` statement](../../../api/ysql/commands/dcl_create_role/) or its alias, the `CREATE USER` statement. Users are roles that have the `LOGIN` privilege granted to them. Roles created with the `SUPERUSER` option in addition to the `LOGIN` option have full access to the database. Superusers can run all of the YSQL statements on any of the database resources.

**NOTE** By default, creating a role does not grant the `LOGIN` or the `SUPERUSER` privileges — these need to be explicitly granted.

#### Create a regular user

To add a new regular user (with non-superuser privileges) named `john`, with the password `PasswdForJohn`, and grant him `LOGIN` privileges, run the following `CREATE ROLE` command.

```postgresql
yugabyte=# CREATE ROLE john WITH LOGIN PASSWORD 'PasswdForJohn';
```

To verify the user account just created, you can run a query like this:

```postgresql
yugabyte=# SELECT role, can_login, is_superuser, member_of FROM system_auth.roles;
```

You should see the following output.

```
 role     | can_login | is_superuser | member_of
-----------+-----------+--------------+-----------
     john |      True |        False |          []
 yugabyte |      True |         True |          []

(2 rows)
```

#### Create a user with SUPERUSER status

The `SUPERUSER` status should be given only to a limited number of users. Applications should generally not access the database using an account that has the superuser privilege.

**NOTE** Only a role with the `SUPERUSER` privilege can create a new role with the `SUPERUSER` privilege, or grant it to an existing role.

To create a superuser `admin` with the `LOGIN` privilege, run the following command using a superuser account:

```postgresql
yugabyte=# CREATE ROLE admin WITH LOGIN SUPERUSER PASSWORD 'PasswdForAdmin';
```

To verify the `admin` account just created, run the following query.

```postgresql
yugabyte=# SELECT rolname, rolsuper, rolcanlogin FROM pg_roles;
```

To see all of the information available in the `pg_roles` table, run `SELECT * from pg_roles`.

You should see a table output similar to this:

```
          rolname          | rolsuper | rolcanlogin 
---------------------------+----------+-------------
 postgres                  | t        | t
 ...
 yugabyte                  | t        | t
 steve                     | f        | t
 john                      | f        | t
(13 rows)
```

In this table, you can see that both `postgres` and `yugabyte` users can log in and have `SUPERUSER` status.

As an easier alternative, you can simply run the `\du` command to see this information in a simpler, easier-to-read format:

```
                                    List of roles
 Role name |                         Attributes                         | Member of  
-----------+------------------------------------------------------------+------------
 john      | Cannot login                                               | {}
 postgres  | Superuser, Create role, Create DB, Replication, Bypass RLS | {}
 steve     | Superuser                                                  | {sysadmin}
 sysadmin  | Create role, Create DB                                     | {}
 yugabyte  | Superuser, Create role, Create DB, Replication, Bypass RLS | {}
 ```

### Connect to ysqlsh using non-default credentials

You can connect to a YSQL cluster with authentication enabled as follows:

```sh
$ ysqlsh -U <username>
```

You will be prompted for a password.

As an example of connecting as a user, you can login with the credentials of the user `john` that you created above by running the following command and entering the password when prompted:

```sh
$ ysqlsh -U john
```

### Edit user accounts

You can edit existing user accounts using the [ALTER ROLE](../../../api/ysql/commands/dcl_alter_role/) command. Note that the role making these changes should have sufficient privileges to modify the target role.

#### Changing password for a user

To change the password for `john` above, you can do:

```postgresql
yugabyte=# ALTER ROLE john PASSWORD 'new-password';
```

#### Granting and removing superuser privileges

In the example above, you can verify that `john` is not a superuser using the following `SELECT` statement:

```postgresql
yugabyte=# SELECT rolname, rolsuper, rolcanlogin FROM pg_roles WHERE rolname='john';
```

```
 rolname | rolsuper | rolcanlogin
---------+----------+-------------
 john    | f        | t
(1 row)
```

Even easier, you can use the YSQL `\du` meta command to display information about the users:

```sh
yugabyte=# \du
```

```
                                      List of roles
   Role name    |                         Attributes                         | Member of  
----------------+------------------------------------------------------------+------------
 john           |                                                            | {}
 postgres       | Superuser, Create role, Create DB, Replication, Bypass RLS | {}
 sysadmin       | Create role, Create DB                                     | {}
 yugabyte       | Superuser, Create role, Create DB, Replication, Bypass RLS | {}
```

Users with `SUPERUSER` status display "Superuser" in the list of attributes for each role.

To grant `SUPERUSER` privileges to `john`, run the following `ALTER ROLE` command.

```postgresql
yugabyte=# ALTER ROLE john SUPERUSER;
```

You can now verify that john is now a superuser by running the `\du` command.

```sh
yugabyte=#\du
```

```
                                      List of roles
   Role name    |                         Attributes                         | Member of  
----------------+------------------------------------------------------------+------------
 john           | Superuser                                                  | {}
 postgres       | Superuser, Create role, Create DB, Replication, Bypass RLS | {}
 sysadmin       | Create role, Create DB                                     | {}
 yugabyte       | Superuser, Create role, Create DB, Replication, Bypass RLS | {}
(1 row)
```

{{< note title="Note" >}}

In YugabyteDB (just as in PostgreSQL), `SUPERUSER` status includes all of the following attributes: `CREATEROLE` ("Create role"), `CREATEDB` ("Create DB"), `REPLICATION` ("Replication"), and BYPASSRLS ("Bypass RLS"). Whether these attributes display or not, all superusers have these attributes.

{{< /note >}}

Similarly, you can revoke superuser privileges by running:

```postgresql
yugabyte=# ALTER ROLE john WITH NOSUPERUSER;
```

### Enable and disable login privileges

In the example above, you can verify that `john` can login to the database by doing the following:

```postgresql
yugabyte=# SELECT role, rolcanlogin FROM pg_roles WHERE role='john';
```

```
 rolname | rolcanlogin
---------+-------------
 john    |  t
(1 rows)
```

To disable login privileges for `john`, run the following command.

```postgresql
yugabyte=# ALTER ROLE john WITH NOLOGIN;
```

You can verify this as follows.

```postgresql
yugabyte=# SELECT rolname, rolcanlogin FROM pg_roles WHERE rolname='john';
```

```
 rolname | rolcanlogin 
---------+-------------
 john    | f
(1 row)
```

Trying to login as `john` using `ysqlsh` will throw the following error.

```sh
yugabyte=# ./bin/ysqlsh -U john
Password for user john:
```

After entering the correct password, John would see the following message:

```
ysqlsh: FATAL:  role "john" is not permitted to log in
```

To re-enable login privileges for `john`, run the following command.

```postgresql
yugabyte=#  ALTER ROLE john WITH LOGIN;
```

### Delete a user

You can delete a user with the [DROP ROLE](../../../api/ysql/commands/dcl_drop_role/) statement.

For example, to drop the user `john` in the above example, run the following command as a superuser:

```postgresql
yugabyte=# DROP ROLE john;
```

You can quickly verify that the `john` role was dropped by running the `\du` command:

```postgresql
yugabyte=# \du
```

```
                                    List of roles
 Role name |                         Attributes                         | Member of  
-----------+------------------------------------------------------------+------------
 postgres  | Superuser, Create role, Create DB, Replication, Bypass RLS | {}
 sysadmin  | Create role, Create DB                                     | {}
 yugabyte  | Superuser, Create role, Create DB, Replication, Bypass RLS | {}
```

## Related topics

- [CREATE ROLE](../../../api/ysql/commands/dcl_create_role/)
- [ALTER ROLE](../../../api/ysql/commands/dcl_alter_role/)
- [DROP ROLE](../../../api/ysql/commands/dcl_drop_role/)
- [GRANT](../../../api/ysql/commands/dcl_grant/)
- [REVOKE](../../../api/ysql/commands/dcl_revoke/)
