---
title: DROP TYPE statement [YSQL]
headerTitle: DROP TYPE
linkTitle: DROP TYPE
description: Use the DROP TYPE statement to remove a user-defined type from the database.
menu:
  latest:
    identifier: ddl_drop_type
    parent: statements
aliases:
  - /latest/api/ysql/commands/ddl_drop_type/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `DROP TYPE` statement to remove a user-defined type from the database.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/drop_type.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/drop_type.diagram.md" /%}}
  </div>
</div>

## Semantics

### *drop_type*

### *type_name*

Specify the name of the user-defined type to drop.

## Examples

Simple example

```plpgsql
yugabyte=# CREATE TYPE feature_struct AS (id INTEGER, name TEXT);
yugabyte=# DROP TYPE feature_struct;
```

`IF EXISTS` example

```plpgsql
yugabyte=# DROP TYPE IF EXISTS feature_shell;
```

`CASCADE` example

```plpgsql
yugabyte=# CREATE TYPE feature_enum AS ENUM ('one', 'two', 'three');
yugabyte=# CREATE TABLE feature_tab_enum (feature_col feature_enum);
yugabyte=# DROP TYPE feature_tab_enum CASCADE;
```

`RESTRICT` example

```plpgsql
yugabyte=# CREATE TYPE feature_range AS RANGE (subtype=INTEGER);
yugabyte=# CREATE TABLE feature_tab_range (feature_col feature_range);
yugabyte=# -- The following should error:
yugabyte=# DROP TYPE feature_range RESTRICT;
yugabyte=# DROP TABLE feature_tab_range;
yugabyte=# DROP TYPE feature_range RESTRICT;
```

## See also

- [`CREATE TYPE`](../ddl_create_type)
