---
title: CREATE OPERATOR statement [YSQL]
headerTitle: CREATE OPERATOR
linkTitle: CREATE OPERATOR
description: Use the CREATE OPERATOR statement to create a new operator.
block_indexing: true
menu:
  v2.1:
    identifier: api-ysql-commands-create-operator
    parent: api-ysql-commands
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `CREATE OPERATOR` statement to create a new operator.

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
    {{% includeMarkdown "../syntax_resources/commands/create_operator,operator_option.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../syntax_resources/commands/create_operator,operator_option.diagram.md" /%}}
  </div>
</div>

## Semantics

See the semantics of each option in the [PostgreSQL docs][postgresql-docs-create-operator].

## Examples

Basic example.

```postgresql
yugabyte=# CREATE OPERATOR @#@ (
             rightarg = int8,
             procedure = numeric_fac
           );
yugabyte=# SELECT @#@ 5;
```

```
 ?column?
----------
      120
```

## See also

- [`DROP OPERATOR`](../ddl_drop_operator)
- [postgresql-docs-create-operator](https://www.postgresql.org/docs/current/sql-createoperator.html)
