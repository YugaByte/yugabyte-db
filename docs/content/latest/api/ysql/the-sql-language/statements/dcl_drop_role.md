---
title: DROP ROLE statement [YSQL]
headerTitle: DROP ROLE
linkTitle: DROP ROLE
description: Use the DROP ROLE statement to remove the specified roles.
menu:
  latest:
    identifier: dcl_drop_role
    parent: statements
aliases:
  - /latest/api/ysql/commands/dcl_drop_role/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `DROP ROLE` statement to remove the specified roles.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/drop_role.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/drop_role.diagram.md" /%}}
  </div>
</div>

Where

- `role_name` is the name of the role to be removed.

To drop a superuser role, you must be a superuser yourself. To drop non-superuser roles, you must have CREATEROLE privilege.

Before dropping the role, you must drop all the objects it owns (or reassign their ownership) and revoke any privileges the role has been granted on other objects. The `REASSIGN OWNED` and `DROP OWNED` commands can be used for this purpose.

It is, however, not necessary to remove role memberships involving the role. `DROP ROLE` automatically revokes any memberships of the target role in other roles, and of other roles in the target role. The other roles are not dropped or affected.

## Example

- Drop a role.

```plpgsql
yugabyte=# DROP ROLE John;
```

## See also

- [`ALTER ROLE`](../dcl_alter_role)
- [`CREATE ROLE`](../dcl_create_role)
- [`GRANT`](../dcl_grant)
- [`REVOKE`](../dcl_revoke)
