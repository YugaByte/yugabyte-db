---
title: YCQL authentication
headerTitle: YCQL authentication
linkTitle: YCQL authentication
description: Identify that your YCQL users are who they say they are.
headcontent: Identify that your YCQL users are who they say they are.
image: /images/section_icons/secure/authentication.png
block_indexing: true
menu:
  v2.1:
    name: Authentication
    identifier: ycql-authentication
    parent: authentication
    weight: 723
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/secure/authentication/ysql-authentication" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>
  <li >
    <a href="/latest/secure/authentication/ycql-authentication" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
  <li>
    <a href="/latest/secure/authentication/yedis-authentication" class="nav-link">
      <i class="icon-redis" aria-hidden="true"></i>
      YEDIS
    </a>
  </li>
</ul>

YCQL authentication is based on roles. Roles can be created with superuser, non-superuser and login privileges. New roles can be created, and existing ones altered or dropped by administrators using YCQL commands.

## 1. Enable YCQL authentication

You can enable access control by starting the `yb-tserver` processes with the `--use_cassandra_authentication=true` flag. Your command should look similar to that shown below:

```
./bin/yb-tserver \
  --tserver_master_addrs <master addresses> \
  --fs_data_dirs <data directories> \
  --use_cassandra_authentication=true \
  >& /home/centos/disk1/yb-tserver.out &
```

You can read more about bringing up the YB-TServers for a deployment in the section on [manual deployment of a YugabyteDB cluster](../../../deploy/manual-deployment/start-tservers/).

## 2. Connect with the default admin credentials

A new YugabyteDB cluster with authentication enabled comes up with a default admin user, the default username/password for this admin user is `cassandra`/`cassandra`. Note that this default user has `SUPERUSER` privilege. You can connect to this cluster using `ycqlsh` as follows:

```sh
$ ycqlsh -u cassandra -p cassandra
```

You should see the cluster connect and the following prompt:

```
Connected to local cluster at 127.0.0.1:9042.
[ycqlsh 5.0.1 | Cassandra 3.9-SNAPSHOT | CQL spec 3.4.2 | Native protocol v4]
Use HELP for help.
cassandra@ycqlsh>
```

## 3. Create a new user

Use the [CREATE ROLE command](../../../api/ycql/ddl_create_role/) to create a new role. Users are roles that have the `LOGIN` privilege granted to them. Roles created with the `SUPERUSER` option in addition to the `LOGIN` option have full access to the database. Superusers can run all the YCQL commands on any of the database resources.

**NOTE** By default, creating a role does not grant the `LOGIN` or the `SUPERUSER` privileges, these need to be explicitly granted.

### Creating a user

For example, to create a regular user `john` with the password `PasswdForJohn` and grant login privileges, run the following command.

```sql
cassandra@ycqlsh> CREATE ROLE IF NOT EXISTS john WITH PASSWORD = 'PasswdForJohn' AND LOGIN = true;
```

If the role `john` already existed, the above statement will not error out since you have added the `IF NOT EXISTS` clause. To verify the user account just created, run the following query:

```sql
cassandra@ycqlsh> SELECT role, can_login, is_superuser, member_of FROM system_auth.roles;
```

You should see the following output.

```
 role      | can_login | is_superuser | member_of
-----------+-----------+--------------+-----------
      john |      True |        False |          []
 cassandra |      True |         True |          []

(2 rows)
```

### Creating a superuser

The `SUPERUSER` status should be given only to a limited number of users. Applications should generally not access the database using an account that has the superuser privilege.

**NOTE** Only a role with the `SUPERUSER` privilege can create a new role with the `SUPERUSER` privilege, or grant it to an existing role.

To create a superuser `admin` with the `LOGIN` privilege, run the following command using a superuser account:

```sql
cassandra@ycqlsh> CREATE ROLE admin WITH PASSWORD = 'PasswdForAdmin' AND LOGIN = true AND SUPERUSER = true;
```

To verify the admin account just created, run the following query.

```sql
cassandra@ycqlsh> SELECT role, can_login, is_superuser, member_of FROM system_auth.roles;
```

You should see the following output.

```
 role      | can_login | is_superuser | member_of
-----------+-----------+--------------+-----------
     admin |      True |         True |          []
      john |      True |        False |          []
 cassandra |      True |         True |          []

(3 rows)
```

## 4. Connect to ycqlsh using non-default credentials

You can connect to a YCQL cluster with authentication enabled as follows:

```sh
$ ycqlsh -u <username> -p <password>
```

Alternatively, you can omit the `-p <password>` above and you will be prompted for a password.

As an example of connecting as a user, you can login with the credentials of the user `john` that you created above by running the following command and entering the password when prompted:

```sh
$ ycqlsh -u john
```

As an example of connecting as the `admin` user, you can run the following command.

```sh
$ ycqlsh -u admin -p PasswdForAdmin
```

## 5. Edit user accounts

You can edit existing user accounts using the [ALTER ROLE](../../../api/ycql/ddl_alter_role/) statement. Note that the role making these changes should have sufficient privileges to modify the target role.

### Changing password for a user

To change the password for `john` above, you can do:

```sql
cassandra@ycqlsh> ALTER ROLE john WITH PASSWORD = 'new-password';
```

### Granting and removing superuser privileges

In the example above, you can verify that `john` is not a superuser:

```sql
cassandra@ycqlsh> SELECT role, can_login, is_superuser, member_of FROM system_auth.roles WHERE role='john';
```

```
 role | can_login | is_superuser | member_of
------+-----------+--------------+-----------
 john |      True |        False |          []

(1 rows)
```

To grant superuser privileges to `john`, run the following command.

```sql
cassandra@ycqlsh> ALTER ROLE john WITH SUPERUSER = true;
```

We can now verify that john is now a superuser.

```sql
cassandra@ycqlsh> SELECT role, can_login, is_superuser, member_of FROM system_auth.roles WHERE role='john';
```

```
 role | can_login | is_superuser | member_of
------+-----------+--------------+-----------
 john |      True |         True |          []

(1 rows)
```

Similarly, you can revoke superuser privileges by running:

```sql
cassandra@ycqlsh> ALTER ROLE john WITH SUPERUSER = false;
```

### Enable and disable login privileges

In the example above, you can verify that `john` is can login to the database by doing the following:

```sql
cassandra@ycqlsh> SELECT role, can_login, is_superuser, member_of FROM system_auth.roles WHERE role='john';
```

```
 role | can_login | is_superuser | member_of
------+-----------+--------------+-----------
 john |      True |        False |          []

(1 rows)
```

To disable login privileges for `john`, run the following command.

```sql
cassandra@ycqlsh> ALTER ROLE john WITH LOGIN = false;
```

You can verify this as follows.

```sql
cassandra@ycqlsh> SELECT role, can_login, is_superuser, member_of FROM system_auth.roles WHERE role='john';
```

```
 role | can_login | is_superuser | member_of
------+-----------+--------------+-----------
 john |     False |        False |          []

(1 rows)
```

Trying to login as `john` using `ycqlsh` will throw the following error.

```sh
$ ycqlsh -u john
Password:
Connection error:
  ... message="john is not permitted to log in"
```

To re-enable login privileges for `john`, run the following command.

```sql
cassandra@ycqlsh>  ALTER ROLE john WITH LOGIN = true;
```

## 6. Change default admin credentials

It is highly recommended to change at least the default password for the superadmin user in real world deployments to keep the database cluster secure.

As an example, let us say you want to change the `cassandra` user's password from `cassandra` to `new_password`. You can do that as follows:

```sql
cassandra@ycqlsh> ALTER ROLE cassandra WITH PASSWORD = 'new_password';
```

Connecting to the cluster with the default password would no longer work:

```
$ bin/ycqlsh -u cassandra -p cassandra
Connection error:
  ... Provided username cassandra and/or password are incorrect ...
```

You can now connect to the cluster using the new password:

```sh
$ ycqlsh -u cassandra -p new_password
```

## 7. Deleting a user

You can delete a user with the [DROP ROLE](../../../api/ycql/ddl_drop_role/) command.

For example, to drop the user `john` in the above example, run the following command as a superuser:

```sql
cassandra@ycqlsh> DROP ROLE IF EXISTS john;
```

You can verify that the `john` role was dropped as follows:

```sql
cassandra@ycqlsh> SELECT role, can_login, is_superuser, member_of FROM system_auth.roles;
```

```
 role      | can_login | is_superuser | member_of
-----------+-----------+--------------+-----------
     admin |      True |         True |          []
 cassandra |      True |         True |          []

(2 rows)
```
