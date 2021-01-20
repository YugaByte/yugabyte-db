---
title: CREATE ROLE statement [YSQL]
headerTitle: CREATE ROLE
linkTitle: CREATE ROLE
description: Use the CREATE ROLE statement to add a role to a YugabyteDB database cluster.
menu:
  latest:
    identifier: dcl_create_role
    parent: statements
aliases:
  - /latest/api/ysql/commands/dcl_create_role/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `CREATE ROLE` statement to add a role to a YugabyteDB database cluster. A role is an entity that can own database objects and have database privileges.
A role can be a user or a group, depending on how it is used. A role with atttribute `LOGIN` can be considered as a "user".
You must have `CREATEROLE` privilege or be a database superuser to use this command.

Note that roles are defined at the YSQL cluster level, and so are valid in all databases in the cluster.

You can use `GRANT`/`REVOKE` commands to set/remove permissions for roles.

## Syntax

<ul class="nav nav-tabs nav-tabs-yb">
  <li >
    <a href="#grammar" class="nav-link active" id="grammar-tab" data-toggle="tab" role="tab" aria-controls="grammar" aria-selected="true">
      <i class="fas fa-file-alt" aria-hidden="true"></i>
      Grammar
    </a>
  </li>
  <li>
    <a href="#diagram" class="nav-link" id="diagram-tab" data-toggle="tab" role="tab" aria-controls="diagram" aria-selected="false">
      <i class="fas fa-project-diagram" aria-hidden="true"></i>
      Diagram
    </a>
  </li>
</ul>

<div class="tab-content">
  <div id="grammar" class="tab-pane fade show active" role="tabpanel" aria-labelledby="grammar-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/create_role,role_option.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/create_role,role_option.diagram.md" /%}}
  </div>
</div>

Where

- `role_name` is the name of the new role.
- `SUPERUSER`, `NOSUPERUSER` determine whether the new role is a “superuser” or not. Superusers can override all access restrictions and should be used with care.
Only roles with SUPERUSER privilege can create other SUPERUSER roles. If not specified, NOSUPERUSER is the default.
- `CREATEDB`, `NOCREATEDB` determine whether the new role can create a database or not. Default is NOCREATEDB.
- `CREATEROLE`, `NOCREATEROLE` determine whether the new role can create other roles or not. Default is NOCREATEROLE.
- `INHERIT`, `NOINHERIT` determine whether the new role inherits privileges of the roles that it is a member of.
Without INHERIT, membership in another role only grants the ability to SET ROLE to that other role. The privileges of the other role are only available after having done so.
If not specified, INHERIT is the default.
- `LOGIN`, `NOLOGIN` determine whether the new role is allowed to login or not. Only roles with login privilege can be used during client connection.
A role with LOGIN can be thought of as a user. If not specified, NOLOGIN is the default. Note that if `CREATE USER` statement is used instead of `CREATE ROLE`, then default is LOGIN.
- `CONNECTION LIMIT` specifies how many concurrent connections the role can make. Default is -1 which means unlimited. This only applies to roles that can login.
- `[ENCRYPTED] PASSWORD` sets the password for the new role. This only applies to roles that can login.
If no password is specified, the password will be set to null and password authentication will always fail for that user.
Note that password is always stored encrypted in system catalogs and the optional keyword ENCRYPTED is only present for compatibility with Postgres.
- `VALID UNTIL` sets a date and time after which the role's password is no longer valid. If this clause is omitted the password will be valid for all time.
- `IN ROLE role_name`, `IN GROUP role_name` lists one or more existing roles to which the new role will be immediately added as a new member. (Note that there is no option to add the new role as an administrator; use a separate GRANT command to do that.)
- `ROLE role_name`, `USER role_name` lists one or more existing roles which are automatically added as members of the new role. (This in effect makes the new role a “group”.)
- `ADMIN role_name` is similar to `ROLE role_name`, but the named roles are added to the new role WITH ADMIN OPTION, giving them the right to grant membership in this role to others.
- `SYSID uid` is ignored and present for compatibility with Postgres.

## Examples

- Create a role that can login.

```plpgsql
yugabyte=# CREATE ROLE John LOGIN;
```

- Create a role that can login and has a password.

```plpgsql
yugabyte=# CREATE ROLE Jane LOGIN PASSWORD 'password';
```

- Create a role that can manage databases and roles.

```plpgsql
yugabyte=# CREATE ROLE SysAdmin CREATEDB CREATEROLE;
```

## See also

- [`ALTER ROLE`](../dcl_alter_role)
- [`DROP ROLE`](../dcl_drop_role)
- [`GRANT`](../dcl_grant)
- [`REVOKE`](../dcl_revoke)
