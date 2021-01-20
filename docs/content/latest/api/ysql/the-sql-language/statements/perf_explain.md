---
title: EXPLAIN statement [YSQL]
headerTitle: EXPLAIN
linkTitle: EXPLAIN
description: Use the EXPLAIN statement to show the execution plan for an statement. If the ANALYZE option is used, the statement will be executed, rather than just planned.
menu:
  latest:
    identifier: perf_explain
    parent: statements
aliases:
  - /latest/api/ysql/commands/perf_explain/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `EXPLAIN` statement to show the execution plan for an statement. If the `ANALYZE` option is used, the statement will be executed, rather than just planned. In that case, execution information (rather than just the planner's estimates) is added to the `EXPLAIN` result.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/explain,option.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/explain,option.diagram.md" /%}}
  </div>
</div>

## Semantics

Where statement is the target statement (see more [here](../dml_select)).

### ANALYZE

Execute the statement and show actual run times and other statistics.

## Examples

Create a sample table.

```plpgsql
yugabyte=# CREATE TABLE sample(k1 int, k2 int, v1 int, v2 text, PRIMARY KEY (k1, k2));
```

Insert some rows.

```plpgsql
yugabyte=# INSERT INTO sample(k1, k2, v1, v2) VALUES (1, 2.0, 3, 'a'), (2, 3.0, 4, 'b'), (3, 4.0, 5, 'c');
```

Check the execution plan for simple select (condition will get pushed down).

```plpgsql
yugabyte=# EXPLAIN SELECT * FROM sample WHERE k1 = 1;
```

```
                           QUERY PLAN
----------------------------------------------------------------
 Foreign Scan on sample  (cost=0.00..112.50 rows=1000 width=44)
(1 row)
```

- Check the execution plan for select with complex condition (second condition requires filtering).

```plpgsql
yugabyte=# EXPLAIN SELECT * FROM sample WHERE k1 = 2 and floor(k2 + 1.5) = v1;
```

```
                           QUERY PLAN
----------------------------------------------------------------
 Foreign Scan on sample  (cost=0.00..125.00 rows=1000 width=44)
   Filter: (floor(((k2)::numeric + 1.5)) = (v1)::numeric)
(2 rows)
```

Check execution with `ANALYZE` option.

```plpgsql
yugabyte=# EXPLAIN ANALYZE SELECT * FROM sample WHERE k1 = 2 and floor(k2 + 1.5) = v1;
```

```
----------------------------------------------------------------------------------------------------------
 Foreign Scan on sample  (cost=0.00..125.00 rows=1000 width=44) (actual time=6.483..6.487 rows=1 loops=1)
   Filter: (floor(((k2)::numeric + 1.5)) = (v1)::numeric)
 Planning time: 2.390 ms
 Execution time: 5.146 ms
(4 rows)
```

## See also

- [`INSERT`](../dml_insert)
- [`SELECT`](../dml_select)
