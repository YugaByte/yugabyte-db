---
title: SET ROLE statement [YSQL]
headerTitle: SET ROLE
linkTitle: SET ROLE
description: Use the SET ROLE statement to set the current user of the current session to be the specified user.
menu:
  latest:
    identifier: dcl_set_role
    parent: statements
aliases:
  - /latest/api/ysql/commands/dcl_set_role/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `SET ROLE` statement to set the current user of the current session to be the specified user.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/set_role,reset_role.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/set_role,reset_role.diagram.md" /%}}
  </div>
</div>

## Semantics

The specified `role_name` must be a role that the current session user is a member of. Superusers can set to any role.
Once the role is set to `role_name`, any further SQL commands will use the privileges available to that role.

To reset the role back to current user, `RESET ROLE` or `SET ROLE NONE` can be used.

## Examples

- Change to new role John.

```plpgsql
yugabyte=# select session_user, current_user;
 session_user | current_user
--------------+--------------
 yugabyte     | yugabyte
(1 row)
yugabyte=# set role john;
SET
yugabyte=# select session_user, current_user;
 session_user | current_user
--------------+--------------
 yugabyte     | john
(1 row)
```

- Changing to new role assumes the privileges available to that role.

```plpgsql
yugabyte=# select session_user, current_user;
 session_user | current_user
--------------+--------------
 yugabyte     | yugabyte
(1 row)
yugabyte=# create database db1;
CREATE DATABASE
yugabyte=# set role john;
SET
yugabyte=# select session_user, current_user;
 session_user | current_user
--------------+--------------
 yugabyte     | john
(1 row)
yugabyte=# create database db2;
ERROR:  permission denied to create database
```

## See also

- [`CREATE ROLE`](../dcl_create_role)
- [`GRANT`](../dcl_grant)
- [`REVOKE`](../dcl_revoke)
