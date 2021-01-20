---
title: DROP OPERATOR statement [YSQL]
headerTitle: DROP OPERATOR
linkTitle: DROP OPERATOR
description: Use the DROP OPERATOR statement to remove an operator.
menu:
  latest:
    identifier: ddl_drop_operator
    parent: statements
aliases:
  - /latest/api/ysql/commands/ddl_drop_operator/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `DROP OPERATOR` statement to remove an operator.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/drop_operator,operator_signature.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/drop_operator,operator_signature.diagram.md" /%}}
  </div>
</div>

## Semantics

See the semantics of each option in the [PostgreSQL docs][postgresql-docs-drop-operator].

## Examples

Basic example.

```plpgsql
yugabyte=# CREATE OPERATOR @#@ (
             rightarg = int8,
             procedure = numeric_fac
           );
yugabyte=# DROP OPERATOR @#@ (NONE, int8);
```

## See also

- [`CREATE OPERATOR`](../ddl_create_operator)
- [postgresql-docs-drop-operator](https://www.postgresql.org/docs/current/sql-dropoperator.html)
