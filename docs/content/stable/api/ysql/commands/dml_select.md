---
title: SELECT statement [YSQL]
headerTitle: SELECT
linkTitle: SELECT
description: Use the SELECT statement to retrieve rows of specified columns that meet a given condition from a table.
block_indexing: true
menu:
  stable:
    identifier: api-ysql-commands-select
    parent: api-ysql-commands
aliases:
  - /stable/api/ysql/dml_select/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `SELECT` statement to retrieve rows of specified columns that meet a given condition from a table. It specifies the columns to be retrieved, the name of the table, and the condition each selected row must satisfy.

The same syntax rules govern a subquery, wherever you might use one—like, for example, in an [`INSERT` statement](../dml_insert/). Certain syntax spots, for example a `WHERE` clause predicate or the actual argument of a function like `sqrt()`, allow only a scalar subquery.

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
    {{% includeMarkdown "../syntax_resources/commands/select,fn_over_window,order_expr.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../syntax_resources/commands/select,fn_over_window,order_expr.diagram.md" /%}}
  </div>
</div>

**Note:** The **fn_over_window** rule denotes the special kind of `SELECT` list item that must be used to invoke a window function and that may be used to invoke an aggregate function. (Window functions are known as analytic functions in the terminology of some database systems.) The dedicated diagram that follows the main diagram for the **select** rule shows the `FILTER` and the `OVER` keywords. You can see that you _cannot_ invoke a function in this way without specifying an `OVER` clause—and that the `OVER` clause requires the specification of the so-called [_window_](../../exprs/window_functions/sql-syntax-semantics/#the-window-definition-rule) that gives this invocation style its name. The `FILTER` clause is optional and may be used _only_ when you invoke an aggregate function in this way. All of this is explained in the major section [Window functions](../../exprs/window_functions/).

## Semantics

- An error is raised if the specified `table_name` does not exist.
- `*` represents all columns.

While the where clause allows a wide range of operators, the exact conditions used in the where clause have significant performance considerations (especially for large datasets).

### *condition*

Specify an expression that evaluates to a Boolean value.

For details on `from_item`, `grouping_element`, and `with_query` see [SELECT](https://www.postgresql.org/docs/10/static/sql-select.html) in the PostgreSQL documentation.

## Examples

Create two sample tables.

```plpgsql
yugabyte=# CREATE TABLE sample1(k1 bigint, k2 float, v text, PRIMARY KEY (k1, k2));
```

```plpgsql
yugabyte=# CREATE TABLE sample2(k1 bigint, k2 float, v text, PRIMARY KEY (k1, k2));
```

Insert some rows.

```plpgsql
yugabyte=# INSERT INTO sample1(k1, k2, v) VALUES (1, 2.5, 'abc'), (1, 3.5, 'def'), (1, 4.5, 'xyz');
```

```plpgsql
yugabyte=# INSERT INTO sample2(k1, k2, v) VALUES (1, 2.5, 'foo'), (1, 4.5, 'bar');
```

Select from both tables using join.

```plpgsql
yugabyte=# SELECT a.k1, a.k2, a.v as av, b.v as bv FROM sample1 a LEFT JOIN sample2 b ON (a.k1 = b.k1 and a.k2 = b.k2) WHERE a.k1 = 1 AND a.k2 IN (2.5, 3.5) ORDER BY a.k2 DESC;
```

```
 k1 | k2  | av  | bv
----+-----+-----+-----
  1 | 3.5 | def |
  1 | 2.5 | abc | foo
(2 rows)
```

## See also

- [`CREATE TABLE`](../ddl_create_table)
- [`INSERT`](../dml_insert)
