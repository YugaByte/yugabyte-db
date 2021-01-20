---
title: CREATE OPERATOR CLASS statement [YSQL]
headerTitle: CREATE OPERATOR CLASS
linkTitle: CREATE OPERATOR CLASS
description: Use the CREATE OPERATOR CLASS statement to create an operator class.
menu:
  latest:
    identifier: ddl_create_operator_class
    parent: statements
aliases:
  - /latest/api/ysql/commands/ddl_create_operator_class/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `CREATE OPERATOR CLASS` statement to create an operator class.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/create_operator_class,operator_class_as.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/create_operator_class,operator_class_as.diagram.md" /%}}
  </div>
</div>

## Semantics

See the semantics of each option in the [PostgreSQL docs][postgresql-docs-create-op-class].  See the
semantics of `strategy_number` and `support_number` in another page of the [PostgreSQL
docs][postgresql-docs-xindex].

## Examples

Basic example.

```plpgsql
yugabyte=# CREATE OPERATOR CLASS my_op_class
           FOR TYPE int4
           USING btree AS
           OPERATOR 1 <,
           OPERATOR 2 <=;
```

## See also

- [`DROP OPERATOR CLASS`](../ddl_drop_operator_class)
- [postgresql-docs-create-op-class](https://www.postgresql.org/docs/current/sql-createopclass.html)
- [postgresql-docs-xindex](https://www.postgresql.org/docs/current/xindex.html)
