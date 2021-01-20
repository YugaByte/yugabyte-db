---
title: PREPARE statement [YSQL]
headerTitle: PREPARE
linkTitle: PREPARE
description: Use the PREPARE statement to create a handle to a prepared statement by parsing, analyzing, and rewriting (but not executing) the target statement.
menu:
  latest:
    identifier: perf_prepare
    parent: statements
aliases:
  - /latest/api/ysql/commands/perf_prepare/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `PREPARE` statement to create a handle to a prepared statement by parsing, analyzing, and rewriting (but not executing) the target statement.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/prepare_statement.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/prepare_statement.diagram.md" /%}}
  </div>
</div>

## Semantics

- The statement in `PREPARE` may (should) contain parameters (e.g. `$1`) that will be provided by the expression list in `EXECUTE`.
- The data type list in `PREPARE` represent the types for the parameters used in the statement.

## Examples

Create a sample table.

```plpgsql
yugabyte=# CREATE TABLE sample(k1 int, k2 int, v1 int, v2 text, PRIMARY KEY (k1, k2));
```

Prepare a simple insert.

```plpgsql
yugabyte=# PREPARE ins (bigint, double precision, int, text) AS 
               INSERT INTO sample(k1, k2, v1, v2) VALUES ($1, $2, $3, $4);
```

Execute the insert twice (with different parameters).

```plpgsql
yugabyte=# EXECUTE ins(1, 2.0, 3, 'a');
```

```plpgsql
yugabyte=# EXECUTE ins(2, 3.0, 4, 'b');
```

Check the results.

```plpgsql
yugabyte=# SELECT * FROM sample ORDER BY k1;
```

```
 k1 | k2 | v1 | v2
----+----+----+----
  1 |  2 |  3 | a
  2 |  3 |  4 | b
(2 rows)
```

## See also

- [`DEALLOCATE`](../perf_deallocate)
- [`EXECUTE`](../perf_execute)
