---
title: CREATE POLICY statement [YSQL]
headerTitle: CREATE POLICY
linkTitle: CREATE POLICY
description: Use the CREATE POLICY statement to create a row level security policy for a table to select, insert, update, or delete rows that match the relevant policy expression.
menu:
  latest:
    identifier: dcl_create_policy
    parent: statements
aliases:
  - /latest/api/ysql/commands/dcl_create_policy/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `CREATE POLICY` statement to create a row level security policy for a table.
A policy grants the permission to select, insert, update, or delete rows that match the relevant policy expression.
Row level security must be enabled on the table using [ALTER TABLE](../ddl_alter_table) for the
policies to take effect.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/create_policy.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/create_policy.diagram.md" /%}}
  </div>
</div>

Where

- `name` is the name of the new policy. This must be distinct from any other policy name for that
  table.
- `table_name` is the name of the table that the policy applies to.
- `PERMISSIVE` / `RESTRICTIVE` specifies that the policy is permissive or restrictive.
While applying policies to a table, permissive policies are combined together using a logical OR operator,
while restrictive policies are combined using logical AND operator. Restrictive policies are used to 
reduce the number of records that can be accessed. Default is permissive.
- `role_name` is the role(s) to which the policy is applied. Default is `PUBLIC` which applies the
  policy to all roles.
- `using_expression` is a SQL conditional expression. Only rows for which the condition returns to
  true will be visible in a `SELECT` and available for modification in an `UPDATE` or `DELETE`.
- `check_expression` is a SQL conditional expression that is used only for `INSERT` and `UPDATE`
  queries. Only rows for which the expression evaluates to true will be allowed in an `INSERT` or
  `UPDATE`. Note that unlike `using_expression`, this is evaluated against the proposed new contents
  of the row.

## Examples

- Create a permissive policy.

```plpgsql
yugabyte=# CREATE POLICY p1 ON document
  USING (dlevel <= (SELECT level FROM user_account WHERE ybuser = current_user));
```

- Create a restricive policy.

```plpgsql
yugabyte=# CREATE POLICY p_restrictive ON document AS RESTRICTIVE TO user_bob
    USING (cid <> 44);
```

- Create a policy with a `CHECK` condition for inserts.

```plpgsql
yugabyte=# CREATE POLICY p2 ON document FOR INSERT WITH CHECK (dauthor = current_user);
```

## See also

- [`ALTER POLICY`](../dcl_alter_policy)
- [`DROP POLICY`](../dcl_drop_policy)
- [`ALTER TABLE`](../ddl_alter_table)
