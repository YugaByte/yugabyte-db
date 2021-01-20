---
title: CREATE GROUP statement [YSQL]
headerTitle: CREATE GROUP
linkTitle: CREATE GROUP
description: Use the CREATE GROUP statement to create a group role. CREATE GROUP is an alias for CREATE ROLE and is used to create a group role.
menu:
  latest:
    identifier: dcl_create_group
    parent: statements
aliases:
  - /latest/api/ysql/commands/dcl_create_group/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `CREATE GROUP` statement to create a group role. `CREATE GROUP` is an alias for [`CREATE ROLE`](../dcl_create_role) and is used to create a group role.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/create_group,role_option.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/create_group,role_option.diagram.md" /%}}
  </div>
</div>


See [`CREATE ROLE`](../dcl_create_role) for more details.

## Examples

- Create a sample group that can manage databases and roles.

```plpgsql
yugabyte=# CREATE GROUP SysAdmin WITH CREATEDB CREATEROLE;
```

## See also

- [`CREATE ROLE`](../dcl_create_role)
- [`GRANT`](../dcl_grant)
- [`REVOKE`](../dcl_revoke)
