---
title: ALTER ROLE statement [YSQL]
headerTitle: ALTER ROLE
linkTitle: ALTER ROLE
description: Use the ALTER ROLE statement to change the attributes of a role (user or group).
menu:
  latest:
    identifier: dcl_alter_role
    parent: statements
aliases:
  - /latest/api/ysql/commands/dcl_alter_role/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `ALTER ROLE` statement to change the attributes of a role (user or group).

Superusers can change the attributes of any role. Roles with `CREATEROLE` privilege can change the attributes of any non-superuser role.
Other roles can only change their own password.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/alter_role,alter_role_option,role_specification,alter_role_rename,alter_role_config,config_setting.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/alter_role,alter_role_option,role_specification,alter_role_rename,alter_role_config,config_setting.diagram.md" /%}}
  </div>
</div>

Where

- `role_specification` specifies the name of the role whose attributes are to be changed or current user or current session user.

- `SUPERUSER`, `NOSUPERUSER` determine whether the role is a “superuser” or not. Superusers can override all access restrictions and should be used with care.
Only roles with SUPERUSER privilege can create other SUPERUSER roles.
- `CREATEDB`, `NOCREATEDB` determine whether the role can create a database or not.
- `CREATEROLE`, `NOCREATEROLE` determine whether the role can create other roles or not.
- `INHERIT`, `NOINHERIT` determine whether the role inherits privileges of the roles that it is a member of.
Without INHERIT, membership in another role only grants the ability to SET ROLE to that other role. The privileges of the other role are only available after having done so.
- `LOGIN`, `NOLOGIN` determine whether new role is allowed to login or not. Only roles with login privilege can be used during client connection.
- `CONNECTION LIMIT` specifies how many concurrent connections the role can make. This only applies to roles that can login.
- `[ENCRYPTED] PASSWORD` sets the password for the role. This only applies to roles that can login.
If no password is specified, the password will be set to null and password authentication will always fail for that user.
Note that password is always stored encrypted in system catalogs and the optional keyword ENCRYPTED is only present for compatibility with Postgres.
- `VALID UNTIL` sets a date and time after which the role's password is no longer valid.

- `config_param` and `config_value` are the name and value of configuration parameters being set.

`ALTER ROLE role_name RENAME TO` can be used to change the name of the role. Note that current session role cannot be renamed.
Because MD5-encrypted passwords use the role name as cryptographic salt, renaming a role clears its password if the password is MD5-encrypted.

`ALTER ROLE SET | RESET config_param` is used to change role's session default for a configuration variable, either for all databases or, when the IN DATABASE clause is specified, only for sessions in the named database. If ALL is specified instead of a role name, this changes the setting for all roles.

## Examples

- Change a role's password.

```plpgsql
yugabyte=# ALTER ROLE John WITH PASSWORD 'new_password';
```

- Rename a role.

```plpgsql
yugabyte=# ALTER ROLE John RENAME TO Jane;
```

- Change default_transaction_isolation session parameter for a role.

```plpgsql
yugabyte=# ALTER ROLE Jane SET default_transaction_isolation='serializable';
```

## See also

- [`CREATE ROLE`](../dcl_create_role)
- [`DROP ROLE`](../dcl_drop_role)
- [`GRANT`](../dcl_grant)
- [`REVOKE`](../dcl_revoke)
