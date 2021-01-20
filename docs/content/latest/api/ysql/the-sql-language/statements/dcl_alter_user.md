---
title: ALTER USER statement [YSQL]
headerTitle: ALTER USER
linkTitle: ALTER USER
description: Use the ALTER USER statement to alter a role. ALTER USER is an alias for ALTER ROLE and is used to alter a role.
menu:
  latest:
    identifier: dcl_alter_user
    parent: statements
aliases:
  - /latest/api/ysql/commands/dcl_alter_user/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `ALTER USER` statement to alter a role. `ALTER USER` is an alias for [`ALTER ROLE`](../dcl_alter_role) and is used to alter a role.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/alter_user,alter_role_option,role_specification,alter_user_rename,alter_user_config,config_setting.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/alter_user,alter_role_option,role_specification,alter_user_rename,alter_user_config,config_setting.diagram.md" /%}}
  </div>
</div>

See [`ALTER ROLE`](../dcl_alter_role) for more details.

## See also

- [`CREATE ROLE`](../dcl_create_role)
- [`DROP ROLE`](../dcl_drop_role)
- [`GRANT`](../dcl_grant)
- [`REVOKE`](../dcl_revoke)
