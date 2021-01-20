---
title: CREATE USER statement [YSQL]
headerTitle: CREATE USER
linkTitle: CREATE USER
description: Use the CREATE USER statement to create a user. The CREATE USER statement is an alias for CREATE ROLE, but creates a role that has LOGIN privileges by default.
menu:
  latest:
    identifier: dcl_create_user
    parent: statements
aliases:
  - /latest/api/ysql/commands/dcl_create_user/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `CREATE USER` statement to create a user. The `CREATE USER` statement is an alias for [`CREATE ROLE`](../dcl_create_role), but creates a role that has LOGIN privileges by default.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/create_user,role_option.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/create_user,role_option.diagram.md" /%}}
  </div>
</div>

## Semantics

See [`CREATE ROLE`](../dcl_create_role) for more details.

## Examples

- Create a sample user with password.

```plpgsql
yugabyte=# CREATE USER John WITH PASSWORD 'password';
```

- Grant John all permissions on the `yugabyte` database.

```plpgsql
yugabyte=# GRANT ALL ON DATABASE yugabyte TO John;
```

- Remove John's permissions from the `yugabyte` database.

```plpgsql
yugabyte=# REVOKE ALL ON DATABASE yugabyte FROM John;
```

## See also

- [`CREATE ROLE`](../dcl_create_role)
- [`GRANT`](../dcl_grant)
- [`REVOKE`](../dcl_revoke)
