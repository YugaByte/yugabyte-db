---
title: DROP AGGREGATE statement [YSQL]
headerTitle: DROP AGGREGATE
linkTitle: DROP AGGREGATE
description: Use the DROP AGGREGATE statement to remove an aggregate.
menu:
  latest:
    identifier: ddl_drop_aggregate
    parent: statements
aliases:
  - /latest/api/ysql/commands/ddl_drop_aggregate/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `DROP AGGREGATE` statement to remove an aggregate.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/drop_aggregate,aggregate_signature.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/drop_aggregate,aggregate_signature.diagram.md" /%}}
  </div>
</div>

## Semantics

See the semantics of each option in the [PostgreSQL docs][postgresql-docs-drop-aggregate].

## Examples

Basic example.

```plpgsql
yugabyte=# CREATE AGGREGATE newcnt(*) (
             sfunc = int8inc,
             stype = int8,
             initcond = '0',
             parallel = safe
           );
yugabyte=# DROP AGGREGATE newcnt(*);
```

`IF EXISTS` example.

```plpgsql
yugabyte=# DROP AGGREGATE IF EXISTS newcnt(*);
yugabyte=# CREATE AGGREGATE newcnt(*) (
             sfunc = int8inc,
             stype = int8,
             initcond = '0',
             parallel = safe
           );
yugabyte=# DROP AGGREGATE IF EXISTS newcnt(*);
```

`CASCADE` and `RESTRICT` example.

```plpgsql
yugabyte=# CREATE AGGREGATE newcnt(*) (
             sfunc = int8inc,
             stype = int8,
             initcond = '0',
             parallel = safe
           );
yugabyte=# CREATE VIEW cascade_view AS
             SELECT newcnt(*) FROM pg_aggregate;
yugabyte=# -- The following should error:
yugabyte=# DROP AGGREGATE newcnt(*) RESTRICT;
yugabyte=# -- The following should error:
yugabyte=# DROP AGGREGATE newcnt(*);
yugabyte=# DROP AGGREGATE newcnt(*) CASCADE;
```

## See also

- [`CREATE AGGREGATE`](../ddl_create_aggregate)
- [postgresql-docs-drop-aggregate](https://www.postgresql.org/docs/current/sql-dropaggregate.html)
