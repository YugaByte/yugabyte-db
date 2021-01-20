---
title: ALTER POLICY statement [YSQL]
headerTitle: ALTER POLICY
linkTitle: ALTER POLICY
description: Use the ALTER POLICY statement to change the definition of a row level security policy.
menu:
  latest:
    identifier: dcl_alter_policy
    parent: statements
aliases:
  - /latest/api/ysql/commands/dcl_alter_policy/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use  the `ALTER POLICY` statement to change the definition of a row level security policy. It can be used to
change the roles that the policy applies to and the `USING` and `CHECK` expressions of the policy.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/alter_policy,alter_policy_rename.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/alter_policy,alter_policy_rename.diagram.md" /%}}
  </div>
</div>

Where

- `name` is the name of the policy being updated.
- `table_name` is the name of the table on which the policy applies.
- `new_name` is the new name of the policy.
- `role_name` is the role(s) to which the policy applies. Use `PUBLIC` if the policy should be
  applied to all roles.
- `using_expression` is a SQL conditional expression. Only rows for which the condition returns to   
  true will be visible in a `SELECT` and available for modification in an `UPDATE` or `DELETE`.      
- `check_expression` is a SQL conditional expression that is used only for `INSERT` and `UPDATE`     
  queries. Only rows for which the expression evaluates to true will be allowed in an `INSERT` or    
  `UPDATE`. Note that unlike `using_expression`, this is evaluated against the proposed new contents 
  of the row.

## Examples

- Rename a policy.

```plpgsql
yugabyte=# ALTER POLICY p1 ON table_foo RENAME TO p2;
```

- Apply policy to all roles.

```plpgsql
yugabyte=# ALTER POLICY p1 ON table_foo TO PUBLIC;
```

## See also

- [`CREATE POLICY`](../dcl_create_policy)
- [`DROP POLICY`](../dcl_drop_policy)
- [`ALTER TABLE`](../ddl_alter_table)
