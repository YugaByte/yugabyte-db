---
title: DROP OPERATOR CLASS statement [YSQL]
headerTitle: DROP OPERATOR CLASS
linkTitle: DROP OPERATOR CLASS
description: Use the DROP OPERATOR CLASS statement to remove an operator class.
menu:
  latest:
    identifier: ddl_drop_operator_class
    parent: statements
aliases:
  - /latest/api/ysql/commands/ddl_drop_operator_class/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `DROP OPERATOR CLASS` statement to remove an operator class.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/drop_operator_class.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/drop_operator_class.diagram.md" /%}}
  </div>
</div>

## Semantics

See the semantics of each option in the [PostgreSQL docs][postgresql-docs-drop-operator-class].

## Examples

Basic example.

```plpgsql
yugabyte=# CREATE OPERATOR CLASS my_op_class
           FOR TYPE int4
           USING btree AS
           OPERATOR 1 <,
           OPERATOR 2 <=;
yugabyte=# DROP OPERATOR CLASS my_op_class USING btree;
```

## See also

- [`CREATE OPERATOR CLASS`](../ddl_create_operator_class)
- [postgresql-docs-drop-operator-class](https://www.postgresql.org/docs/current/sql-dropopclass.html)
