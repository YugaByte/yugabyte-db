---
title: DROP FUNCTION statement [YSQL]
headerTitle: DROP FUNCTION
linkTitle: DROP FUNCTION
description: Remove a function from a database.
menu:
  latest:
    identifier: ddl_drop_function
    parent: statements
aliases:
  - /latest/api/ysql/commands/ddl_drop_function/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `DROP FUNCTION` statement to remove a function from a database.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/drop_function,argtype_decl.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/drop_function,argtype_decl.diagram.md" /%}}
  </div>
</div>

## Semantics

- An error will be thrown if the function does not exist unless `IF EXISTS` is used. Then a notice is issued instead.

- `RESTRICT` is the default and it will not drop the function if any objects depend on it.

- `CASCADE` will drop any objects that transitively depend on the function.

## Examples

```plpgsql
DROP FUNCTION IF EXISTS inc(i integer), mul(integer, integer) CASCADE;
```

## See also

- [`CREATE FUNCTION`](../ddl_create_function)
- [`DROP PROCEDURE`](../ddl_drop_procedure)
- [`DROP TRIGGER`](../ddl_drop_trigger)
