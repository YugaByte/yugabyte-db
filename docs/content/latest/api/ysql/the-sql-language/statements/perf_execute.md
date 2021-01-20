---
title: EXECUTE statement [YSQL]
headerTitle: EXECUTE
linkTitle: EXECUTE
description: Use the EXECUTE statement to execute a previously prepared statement. 
menu:
  latest:
    identifier: perf_execute
    parent: statements
aliases:
  - /latest/api/ysql/commands/perf_execute/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `EXECUTE` statement to execute a previously prepared statement. This separation is a performance optimization because a prepared statement would be executed many times with different values while the syntax and semantics analysis and rewriting are done only once during `PREPARE` processing.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/execute_statement.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/execute_statement.diagram.md" /%}}
  </div>
</div>

## Semantics

### *name*

Specify the name of the prepared statement to execute.

### *expression*

Specify the expression. Each expression in `EXECUTE` must match with the corresponding data type from `PREPARE`.

## Examples

- Create a sample table.

```plpgsql
yugabyte=# CREATE TABLE sample(k1 int, k2 int, v1 int, v2 text, PRIMARY KEY (k1, k2));
```

- Prepare a simple insert.

```plpgsql
yugabyte=# PREPARE ins (bigint, double precision, int, text) AS 
               INSERT INTO sample(k1, k2, v1, v2) VALUES ($1, $2, $3, $4);
```

- Execute the insert twice (with different parameters).

```plpgsql
yugabyte=# EXECUTE ins(1, 2.0, 3, 'a');
```

```plpgsql
yugabyte=# EXECUTE ins(2, 3.0, 4, 'b');
```

- Check the results.

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
- [`PREPARE`](../perf_prepare)
