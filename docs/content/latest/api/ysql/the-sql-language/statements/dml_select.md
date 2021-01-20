---
title: SELECT statement [YSQL]
headerTitle: SELECT
linkTitle: SELECT
description: Use the SELECT statement to retrieve rows of specified columns that meet a given condition from a table.
menu:
  latest:
    identifier: dml_select
    parent: statements
aliases:
  - /latest/api/ysql/commands/dml_select/
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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/select,with_clause_substatement_defn,fn_over_window,ordinary_aggregate_fn_invocation,within_group_aggregate_fn_invocation,grouping_element,order_expr.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/select,with_clause_substatement_defn,fn_over_window,ordinary_aggregate_fn_invocation,within_group_aggregate_fn_invocation,grouping_element,order_expr.diagram.md" /%}}
  </div>
</div>

See the section [The WITH clause (Common Table Expression)](../../with-clause/) for mor information about the semantics of the `with_clause_substatement_defn` grammar rule.

## Semantics

- An error is raised if the specified `table_name` does not exist.
- `*` represents all columns.

While the where clause allows a wide range of operators, the exact conditions used in the where clause have significant performance considerations (especially for large datasets).

For details on `from_item` and `with_query` see [SELECT](https://www.postgresql.org/docs/10/static/sql-select.html) in the PostgreSQL documentation.

The `fn_over_window` rule denotes the special kind of `SELECT` list item that must be used to invoke a window function and that may be used to invoke an aggregate function. (Window functions are known as analytic functions in the terminology of some SQL database systems.) The dedicated diagram that follows the main diagram for the `select` rule shows the `FILTER` and the `OVER` keywords. You can see that you _cannot_ invoke a function in this way without specifying an `OVER` clause—and that the `OVER` clause requires the specification of the so-called [_window_](../../../exprs/window_functions/invocation-syntax-semantics/#the-window-definition-rule) that gives this invocation style its name. The `FILTER` clause is optional and may be used _only_ when you invoke an aggregate function in this way. All of this is explained in the [Window function invocation—SQL syntax and semantics](../../../exprs/window_functions/invocation-syntax-semantics/) section within the major section [Window functions](../../../exprs/window_functions/).

The `ordinary_aggregate_fn_invocation` rule and the `within_group_aggregate_fn_invocation` rule denote the special kinds of `SELECT` list item that are used to invoke an aggregate function (when it isn't invoked as a window function). When an aggregate function is invoked in either of these two ways, it's very common to do so in conjunction with the `GROUP BY` and `HAVING` clauses. All of this is explained in the [Aggregate function invocation—SQL syntax and semantics](../../../exprs/aggregate_functions/invocation-syntax-semantics/) section within the major section [Aggregate functions](../../../exprs/aggregate_functions/).

When you understand the story of the invocation of these two kinds of functions from the accounts in the [Window functions](../../../exprs/window_functions/) section and the [Aggregate functions](../../../exprs/aggregate_functions/) section, you can use the `\df` metacommand in `ysqlsh` to discover the status of a particular function, thus:

```
\df row_number

... |          Argument data types           |  Type
... +----------------------------------------+--------
... |                                        | window


\df rank

... |          Argument data types           |  Type
... +----------------------------------------+--------
... |                                        | window
... | VARIADIC "any" ORDER BY VARIADIC "any" | agg


\df avg

... |          Argument data types           |  Type
... +----------------------------------------+--------
... | bigint                                 | agg
... | <other data types>                     | agg
```

- A function whose type is listed as _"window"_ can be invoked _only_ as a window function. See this account of [`row_number()`](../../../exprs/window_functions/function-syntax-semantics/row-number-rank-dense-rank/#row-number).

- A function whose type is listed _both_ as _"window"_ and as _agg_ can be invoked:

  - _either_ as a window function using the `fn_over_window` syntax—see this account of [`rank()`](../../../exprs/window_functions/function-syntax-semantics/row-number-rank-dense-rank/#rank)

  - _or_ as a so-called [within-group hypothetical-set aggregate function](../../../exprs/aggregate_functions/function-syntax-semantics/#within-group-hypothetical-set-aggregate-functions) using the `within_group_aggregate_fn_invocation` syntax—see this account of [`rank()`](../../../exprs/aggregate_functions/function-syntax-semantics/rank-dense-rank-percent-rank-cume-dist/#rank).

- A function whose type is listed only as _"agg"_ can, in fact, be invoked _either_ as an aggregate function using the `ordinary_aggregate_fn_invocation` syntax _or_ as a window function using the `fn_over_window` syntax. The `avg()` function is described in the "Aggregate functions" major section section in the subsection [`avg()`, `count()`, `max()`, `min()`, `sum()`](../../../exprs/aggregate_functions/function-syntax-semantics/avg-count-max-min-sum/). See its subsections [`GROUP BY` syntax](../../../exprs/aggregate_functions/function-syntax-semantics/avg-count-max-min-sum/#group-by-syntax) and [`OVER` syntax](../../../exprs/aggregate_functions/function-syntax-semantics/avg-count-max-min-sum/#over-syntax) for, respectively, the `ordinary_aggregate_fn_invocation` and the `fn_over_window` invocation alternatives.

- Notice that the three functions [`mode()`](../../../exprs/aggregate_functions/function-syntax-semantics/mode-percentile-disc-percentile-cont/#mode), [`percentile_disc()`](../../../exprs/aggregate_functions/function-syntax-semantics/mode-percentile-disc-percentile-cont/#percentile-disc-percentile-cont), and [`percentile_cont()`](../../../exprs/aggregate_functions/function-syntax-semantics/mode-percentile-disc-percentile-cont/#percentile-disc-percentile-cont) are exceptions to this general rule (and they are the _only_ exceptions). These functions are referred to as [within-group ordered-set aggregate functions](../../../exprs/aggregate_functions/function-syntax-semantics/#within-group-ordered-set-aggregate-functions). `\df` lists the type of these functions only as _"agg"_. But these _cannot_ be invoked as window functions. The attempt causes this error:

  ```
  42809: WITHIN GROUP is required for ordered-set aggregate mode
  ```

**Note:** The documentation in the [Aggregate functions](../../../exprs/aggregate_functions/) major section usually refers to the syntax that the `ordinary_aggregate_fn_invocation` rule and the `within_group_aggregate_fn_invocation` rule jointly govern as the [`GROUP BY` syntax](../../../exprs/aggregate_functions/function-syntax-semantics/avg-count-max-min-sum/#group-by-syntax) because it's these two syntax variants (and _only_ these two) can be used together with the `GROUP BY` clause (and therefore the `HAVING` clause). And it usually refers to the syntax that the `fn_over_window` rule governs as the [`OVER` syntax](../../../exprs/aggregate_functions/function-syntax-semantics/avg-count-max-min-sum/#over-syntax) because this syntax variant _requires_ the use of the `OVER` clause. Moreover, the use of the `GROUP BY` clause (and therefore the `HAVING` clause) is illegal with this syntax variant. 

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
