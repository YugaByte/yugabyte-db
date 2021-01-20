---
title: ALTER GROUP statement [YSQL]
headerTitle: ALTER GROUP
linkTitle: ALTER GROUP
description: Use the `ALTER GROUP` statement to alter attributes for a group (role).
menu:
  latest:
    identifier: dcl_alter_group
    parent: statements
aliases:
  - /latest/api/ysql/commands/dcl_alter_group/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `ALTER GROUP` statement to alter attributes for a group (role).
This is added for compatibility with Postgres. Its usage is discouraged. [`ALTER ROLE`](../dcl_alter_role) is the preferred way to change attributes of a role.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/alter_group,role_specification,alter_group_rename.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/alter_group,role_specification,alter_group_rename.diagram.md" /%}}
  </div>
</div>


See [`ALTER ROLE`](../dcl_alter_role) for more details.

`ALTER GROUP` can be used to add or remove roles from a group. Please use [`GRANT`](../dcl_grant) or [`REVOKE`](../dcl_revoke) instead.
It can also be used to rename a role.

## See also

- [`ALTER ROLE`](../dcl_alter_role)
- [`CREATE ROLE`](../dcl_create_role)
- [`DROP ROLE`](../dcl_drop_role)
- [`GRANT`](../dcl_grant)
- [`REVOKE`](../dcl_revoke)
