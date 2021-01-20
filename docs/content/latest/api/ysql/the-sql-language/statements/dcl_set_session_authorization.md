---
title: SET SESSION AUTHORIZATION statement [YSQL]
headerTitle: SET SESSION AUTHORIZATION
linkTitle: SET SESSION AUTHORIZATION
description: Use the SET SESSION AUTHORIZATION statement to set the current user and session user of the current session to be the specified user.
menu:
  latest:
    identifier: dcl_set_session_authorization
    parent: statements
aliases:
  - /latest/api/ysql/commands/dcl_set_session_authorization/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `SET SESSION AUTHORIZATION` statement to set the current user and session user of the current session to be the specified user.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/set_session_authorization,reset_session_authorization.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/set_session_authorization,reset_session_authorization.diagram.md" /%}}
  </div>
</div>

## Semantics

Session user can only be changed by superusers.
Once the session user is set to `role_name`, any further SQL commands will use the privileges available to that role.

To reset the session user back to current authenticated user, `RESET SESSION AUTHORIZATION` or `SET SESSION AUTHORIZATION DEFAULT` can be used.

## Examples

- Set session user to John.

```plpgsql
yugabyte=# select session_user, current_user;
 session_user | current_user
--------------+--------------
 yugabyte     | yugabyte
(1 row)
yugabyte=# set session authorization john;
SET
yugabyte=# select session_user, current_user;
 session_user | current_user
--------------+--------------
 john     | john
(1 row)
```

## See also

- [`CREATE ROLE`](../dcl_create_role)
- [`GRANT`](../dcl_grant)
- [`REVOKE`](../dcl_revoke)
