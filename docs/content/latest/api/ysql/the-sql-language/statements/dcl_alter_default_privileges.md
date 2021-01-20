---
title: ALTER DEFAULT PRIVILEGES statement [YSQL]
headerTitle: ALTER DEFAULT PRIVILEGES
linkTitle: ALTER DEFAULT PRIVILEGES
description: Use the ALTER DEFAULT PRIVILEGES statement to define the default access privileges.
menu:
  latest:
    identifier: dcl_alter_default_privileges
    parent: statements
aliases:
  - /latest/api/ysql/commands/dcl_alter_default_privileges/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `ALTER DEFAULT PRIVILEGES` statement to define the default access privileges.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/alter_default_priv,abbr_grant_or_revoke,a_grant_table,a_grant_seq,a_grant_func,a_grant_type,a_grant_schema,a_revoke_table,a_revoke_seq,a_revoke_func,a_revoke_type,a_revoke_schema,grant_table_priv,grant_seq_priv,grant_role_spec.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/alter_default_priv,abbr_grant_or_revoke,a_grant_table,a_grant_seq,a_grant_func,a_grant_type,a_grant_schema,a_revoke_table,a_revoke_seq,a_revoke_func,a_revoke_type,a_revoke_schema,grant_table_priv,grant_seq_priv,grant_role_spec.diagram.md" /%}}
  </div>
</div>

## Semantics

`ALTER DEFAULT PRIVILEGES` defines the privileges for objects created in future. It does not affect objects that are already created.

Users can change default privileges only for objects that are created by them or by roles that they are a member of.

## Examples

- Grant SELECT privilege to all tables that are created in schema marketing to all users.

```plpgsql
yugabyte=# ALTER DEFAULT PRIVILEGES IN SCHEMA marketing GRANT SELECT ON TABLES TO PUBLIC;
```

- Revoke INSERT privilege on all tables from user john.

```plpgsql
yugabyte=# ALTER DEFAULT PRIVILEGES REVOKE INSERT ON TABLES FROM john;
```

## See also

- [`CREATE ROLE`](../dcl_create_role)
- [`GRANT`](../dcl_grant)
- [`REVOKE`](../dcl_revoke)
