---
title: Window function syntax and semantics
linkTitle: Invocation syntax and semantics
headerTitle: Window function invocation—SQL syntax and semantics
description: This section specifies the syntax and semantics of the OVER clause and the WINDOW clause. You may also invoke aggregate functions t_is way.
menu:
  latest:
    identifier: window-functions-aggregate-functions-syntax-semantics
    parent: window-functions
    weight: 20
isTocNested: true
showAsideToc: true
---

{{< note title="The rules described in this section also govern the invocation of aggregate functions." >}}

The dedicated [Aggregate functions](../../aggregate_functions/) section explains that one kind of aggregate function—so-called ordinary aggregate functions, exemplified by [`avg()`](../../aggregate_functions/function-syntax-semantics/avg-count-max-min-sum/#avg) and [`count()`](../../aggregate_functions/function-syntax-semantics/avg-count-max-min-sum/#count)—can optionally be invoked using the identical syntax that you use to invoke window functions. That dedicated section has many examples. See also the sections [Using the aggregate function avg() to compute a moving average](../functionality-overview/#using-the-aggregate-function-avg-to-compute-a-moving-average) and [Using the aggregate function sum() with the OVER clause](../functionality-overview/#using-the-aggregate-function-sum-with-the-over-clause) in the present Window functions main section.
{{< /note >}}

{{< note title="A note on orthography" >}}

Notice these three different orthography styles:

- `OVER` is a keyword that names a clause. You write such a keyword in a SQL statement.

- [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) is the name of a rule within the overall SQL grammar. You never type such a name in a SQL statement. It is written in bold lower case with underscores, as appropriate, between the English words. Because such a rule is always shown as a link, you can jump directly to the rule in the [Grammar Diagrams](../../../syntax_resources/grammar_diagrams/#abort) page. This page shows every single one of the SQL rules. It so happens that the [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) rule starts with the keyword `WINDOW` and might therefore, according to the context of use, be referred to alternatively as the `WINDOW` clause. 

- [_window frame_](./#frame-clause-semantics-for-window-functions) is a pure term of art. It is written in italic lower case with spaces, as appropriate, between the English words. You neither write it in a SQL statement nor use it to look up anything in the [Grammar Diagrams](../../../syntax_resources/grammar_diagrams/#abort) page. Because such a term of art is always shown as a link, you can jump directly to its definition within this _"Window function invocation—SQL syntax and semantics"_ page.

{{< /note >}}

## Syntax

### Reproduced from the SELECT statement section

The following three diagrams, [`select_start`](../../../syntax_resources/grammar_diagrams/#select-start), [`WINDOW` clause](../../../syntax_resources/grammar_diagrams/#window-clause), and [`fn_over_window`](../../../syntax_resources/grammar_diagrams/#fn-over-window) rule, are reproduced from the section that describes the [`SELECT` statement](../../../the-sql-language/statements/dml_select/).

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
    {{% includeMarkdown "../../syntax_resources/exprs/window_functions/select_start,window_clause,fn_over_window.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/exprs/window_functions/select_start,window_clause,fn_over_window.diagram.md" /%}}
  </div>
</div>

### Definition of the window_definition rule

As promised in the `SELECT` statement section, this section explains the [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) rule and its use as the argument of either the `OVER` keyword or the `WINDOW` keyword.

A [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) can be used only at these two syntax spots, within the enclosing syntax of a subquery.

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
    {{% includeMarkdown "../../syntax_resources/exprs/window_functions/window_definition.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/exprs/window_functions/window_definition.diagram.md" /%}}
  </div>
</div>

### The frame_clause

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
    {{% includeMarkdown "../../syntax_resources/exprs/window_functions/frame_clause,frame_bounds,frame_start,frame_end,frame_bound,frame_exclusion.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/exprs/window_functions/frame_clause,frame_bounds,frame_start,frame_end,frame_bound,frame_exclusion.diagram.md" /%}}
  </div>
</div>

## Semantics

### The fn_over_window rule

A window function can be invoked only at the syntax spot in a subquery that the diagram for the [`select_start`](../../../syntax_resources/grammar_diagrams/#select-start) rule shows. An [aggregate function](../../aggregate_functions/) _may_ be invoked in this way as an alternative to its more familiar invocation as a regular `SELECT` list item in conjunction with the `GROUP BY` clause. (The invocation of an aggregate function in conjunction with the `GROUP BY` clause is governed by the `ordinary_aggregate_fn_invocation` rule or the `within_group_aggregate_fn_invocation` rule.)

The number, data types, and meanings of a window function's formal parameters are function-specific. The eleven window functions are classified into functional groups, and summarized, in the two tables at the end of the section [Signature and purpose of each window function](../function-syntax-semantics/). Each entry links to the formal account of the function which also provides runnable code examples.

Notice that, among the dedicated window functions (as opposed to aggreagte functions that may be invoked as window functions), only [`ntile()`](../function-syntax-semantics/percent-rank-cume-dist-ntile/#ntile) takes an argument. Every other dedicated window function is invoked with an empty parentheses pair. Some aggregate functions (like, for example, [`jsonb_object_agg()`](../../aggregate_functions/function-syntax-semantics/array-string-jsonb-jsonb-object-agg/#jsonb-object-agg)) take more than one argument. When an aggregate function is invoke as a window function, the keyword `DISTINCT` is not allowed within the parenthesized list of arguments. The attempt causes this error:

```
0A000: DISTINCT is not implemented for window functions
```

### The window_definition rule

The syntax diagram for the [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) shows that it uses three complementary specifications:

- The `PARTITION BY` clause defines the maximal subsets, of what the subquery-level `WHERE` clause defines, that are operated upon, in turn, by a window function (or by an aggregate function in window mode). Tautologically, this maximal subset is referred to as the [_window_](./#the-window-definition-rule). In the limit, when the `PARTITION BY` clause is omitted, the maximal subset is identical with what the `WHERE` clause defines.
- The window `ORDER BY` clause defines how the rows are to be ordered within the [_window_](./#the-window-definition-rule).
- The [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause) defines a further narrowing of the [_window_](./#the-window-definition-rule), referred to as the [_window frame_](./#frame-clause-semantics-for-window-functions). The [_window frame_](./#frame-clause-semantics-for-window-functions) is anchored to the current row within the [_window_](./#the-window-definition-rule). In the degenerate case, the [_window frame_](./#frame-clause-semantics-for-window-functions) coincides with the [_window_](./#the-window-definition-rule) and is therefore insensitive to the position of the current row.

In summary, the [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) defines the [_window_](./#the-window-definition-rule) as the scope within which a function's meaning (window function or aggregate function in window mode) is defined. The [_window_](./#the-window-definition-rule) is then further characterized by the ordering of its rows, the extent of the [_window frame_](./#frame-clause-semantics-for-window-functions), and how this moves with the current row.

### The FILTER clause

The `FILTER` clause's `WHERE` clause has the same syntax and semantics as it does at the regular `WHERE` clause syntax spot immediately after a subquery's `FROM` list. Notice that the `FILTER` clause is legal only for the invocation of an aggregate function. Here is an example:

```plpgsql
select
  class,
  k,
  count(*)
    filter(where k%2 = 0)
    over (partition by class)
  as n
from t1
order by class, k;
```

If you want to run this, then create a data set using the `ysqlsh` script that [table t1](../function-syntax-semantics/data-sets/table-t1/) presents.

Using the `FILTER` clause in the invocation of a window function causes this compilation error:

```
0A000: FILTER is not implemented for non-aggregate window functions
```

### The PARTITION BY clause

The `PARTITION BY` clause groups the rows that the subquery defines into [_windows_](./#the-window-definition-rule), which are processed separately by the window function. (This holds, too, when an aggregate function is invoked in this way.) It works similarly to a query-level `GROUP BY` clause, except that its expressions are always just expressions and cannot be output-column names or numbers. If the `PARTITION BY` clause is omitted, then all rows are treated as a single [_window_](./#the-window-definition-rule).

### The window ORDER BY clause

The window `ORDER BY` clause determines the order in which the rows of a [_window_](./#the-window-definition-rule) are processed by the window function. It works similarly to a query-level `ORDER BY` clause; but it cannot use output-column names or numbers. If the window `ORDER BY` clause is omitted, then rows are processed in an unspecified order so that the results of any window function invoked in this way would be unpredictable and therefore meaningless. Aggregation functions invoked in this way might be sensitive to what the window `ORDER BY` clause says. This will be the case when, for example, the [_window frame_](./#frame-clause-semantics-for-window-functions) is smaller than the whole [_window_](./#the-window-definition-rule) and moves with the current row. The section [Using the aggregate function avg() to compute a moving average](../functionality-overview/#using-the-aggregate-function-avg-to-compute-a-moving-average) provides an example.

### The frame_clause

The [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause) has many variants. Only one basic variant is needed in the `OVER` clause that you use to invoke a window function. The other variants are useful in the `OVER` clause that you use to invoke an aggregate function. For completeness, those variants are described on this page.

#### frame_clause semantics for window functions

The [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause) specifies the set of rows constituting the so-called [_window frame_](./#frame-clause-semantics-for-window-functions). In general, this will be a subset of the rows in the current [_window_](./#the-window-definition-rule). Look at the two tables at the end of the section [Signature and purpose of each window function](../function-syntax-semantics/).

- The functions in the first group, [Window functions that return an "int" or "double precision" value as a "classifier" of the rank of the row within its window](../function-syntax-semantics/#window-functions-that-return-an-int-or-double-precision-value-as-a-classifier-of-the-rank-of-the-row-within-its-window), are not sensitive to what the [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause) specifies and always use all of the rows in the current [_window_](./#the-window-definition-rule). Yugabyte recommends that you therefore omit the [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause) in the `OVER` clause that you use to invoke these functions.

- The functions in the second group, [Window functions that return columns of another row within the window](../function-syntax-semantics/#window-functions-that-return-column-s-of-another-row-within-the-window), make obvious sense when the scope within which the specified row is found is the entire [_window_](./#the-window-definition-rule). If you have one of the very rare use cases where the output that you want is produced by a different [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause), then specify what you want explicitly. Otherwise, because it isn't the default, you must specify that the [_window frame_](./#frame-clause-semantics-for-window-functions) includes the entire current [_window_](./#the-window-definition-rule) like this:
  
  ```
  range between unbounded preceding and unbounded following
  ```

Use cases where the [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause)'s many other variants are useful arise when an aggregate function is invoked using the `OVER` clause. One example is given in the section [Using the aggregate function `avg()` to compute a moving average](../#using-the-aggregate-function-avg-to-compute-a-moving-average). Another example, that uses `count(*)`, is given in the code that explains the meaning of the [`percent_rank()`](../percent-rank-cume-dist-ntile/#percent-rank) function. Otherwise, see the main [Aggregate functions](../../aggregate_functions) section.

#### frame_clause semantics for aggregate functions

The [_window frame_](./#frame-clause-semantics-for-window-functions) can be specified in `RANGE`, `ROWS` or `GROUPS` mode; in each case, it runs from the [`frame_start`](../../../syntax_resources/grammar_diagrams/#frame-start) to the [`frame_end`](../../../syntax_resources/grammar_diagrams/#frame-end). If [`frame_end`](../../../syntax_resources/grammar_diagrams/#frame-end) is omitted, then the end defaults to `CURRENT ROW`.

A [`frame_start`](../../../syntax_resources/grammar_diagrams/#frame-start) of `UNBOUNDED PRECEDING` means that the [_window frame_](./#frame-clause-semantics-for-window-functions) starts with the first row of the [_window_](./#the-window-definition-rule). Similarly, a [`frame_end`](../../../syntax_resources/grammar_diagrams/#frame-end) of `UNBOUNDED FOLLOWING` means that the [_window frame_](./#frame-clause-semantics-for-window-functions) ends with the last row of the [_window_](./#the-window-definition-rule).

In `RANGE` or `GROUPS` mode, a [`frame_start`](../../../syntax_resources/grammar_diagrams/#frame-start) of `CURRENT ROW` means that the [_window frame_](./#frame-clause-semantics-for-window-functions) starts with the first member of the current row's _peer group_. A _peer group_ is a set of rows that the window `ORDER BY` clause sorts with the same rank as the current row. And a [`frame_end`](../../../syntax_resources/grammar_diagrams/#frame-end) of `CURRENT ROW` means that the [_window frame_](./#frame-clause-semantics-for-window-functions) ends with the last row in the current row's _peer group_. In `ROWS` mode, `CURRENT ROW` simply means the current row.

For the [`offset`](../../../syntax_resources/grammar_diagrams/#offset) `PRECEDING` and [`offset`](../../../syntax_resources/grammar_diagrams/#offset) `FOLLOWING` modes of the [`frame_start`](../../../syntax_resources/grammar_diagrams/#frame-start) and [`frame_end`](../../../syntax_resources/grammar_diagrams/#frame-end) clauses, the [`offset`](../../../syntax_resources/grammar_diagrams/#offset) argument must be an expression that doesn't include any variables, aggregate functions, or window functions. The meaning of the [`offset`](../../../syntax_resources/grammar_diagrams/#offset) value depends on the `RANGE | ROWS | GROUPS` mode:

- In `ROWS` mode, the [`offset`](../../../syntax_resources/grammar_diagrams/#offset) value must be a `NOT NULL`, non-negative integer. This brings the meaning that the [_window frame_](./#frame-clause-semantics-for-window-functions) starts or ends the specified number of rows before or after the current row.

- In `GROUPS` mode, the [`offset`](../../../syntax_resources/grammar_diagrams/#offset) value must again be a `NOT NULL`, non-negative integer. Here, this brings the meaning that the [_window frame_](./#frame-clause-semantics-for-window-functions) starts or ends the specified number of _peer groups_ before or after the current row's _peer group_. Recall that there's always a logical requirement to include a window `ORDER BY` clause in the window definition that is used to invoke a window function. In `GROUPS` mode, whatever is your intended use of the window definition, you get this error if it doesn't include a window `ORDER BY` clause:

  ```
  42P20: GROUPS mode requires an ORDER BY clause
  ```

- In `RANGE` mode, these options require that the window `ORDER BY` clause specify exactly one column. The [`offset`](../../../syntax_resources/grammar_diagrams/#offset) value specifies the maximum difference between the value of that column in the current row and its value in the preceding or following rows of the [_window frame_](./#frame-clause-semantics-for-window-functions). The [`offset`](../../../syntax_resources/grammar_diagrams/#offset) expression must yield a value whose data type depends upon that of the ordering column. For numeric ordering columns (like `int`, `double precision`, and so on), it is typically of the same data type as the ordering column; but for date-time ordering columns it is an `interval`. For example, if the ordering column is `date` or `timestamp`, you could specify `RANGE BETWEEN '1 day' PRECEDING AND '10 days' FOLLOWING`. Here too, the [`offset`](../../../syntax_resources/grammar_diagrams/#offset) value must be `NOT NULL` and non-negative. The meaning of “non-negative” depends on the data type.

In all cases, the distance to the start and end of the [_window frame_](./#frame-clause-semantics-for-window-functions) is limited by the distance to the start and end of the [_window_](./#the-window-definition-rule), so that for rows near the [_window_](./#the-window-definition-rule) boundaries, the [_window frame_](./#frame-clause-semantics-for-window-functions) might contain fewer rows than elsewhere.

Notice that in both `ROWS` and `GROUPS` mode, `0 PRECEDING` and `0 FOLLOWING` is equivalent to `CURRENT ROW`. This normally holds in `RANGE` mode too, for an appropriate meaning of “zero” specific to the data type.

The [`frame_exclusion`](../../../syntax_resources/grammar_diagrams/#frame-exclusion) clause allows rows around the current row to be excluded from the [_window frame_](./#frame-clause-semantics-for-window-functions), even if they would be included according to what the [`frame_start`](../../../syntax_resources/grammar_diagrams/#frame-start) and [`frame_end`](../../../syntax_resources/grammar_diagrams/#frame-end) clauses say.

- `EXCLUDE CURRENT ROW` excludes the current row from the [_window frame_](./#frame-clause-semantics-for-window-functions).
- `EXCLUDE GROUP` excludes all the rows in the current row's _peer group_.
- `EXCLUDE TIES` excludes any peers of the current row, but not the current row itself.
- `EXCLUDE NO OTHERS` simply specifies explicitly the default behavior of not excluding the current row or its peers.

Omitting the [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause) is the same as specifying

```
RANGE UNBOUNDED PRECEDING
```

and this means the same as

```
RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
```

If the window `ORDER BY` clause is specified, then this default meaning sets the [_window frame_](./#frame-clause-semantics-for-window-functions) to be all rows from the [_window_](./#the-window-definition-rule) start up through the last row in the current row's _peer group_. And if the window `ORDER BY` clause is omitted this means that all rows of the [_window_](./#the-window-definition-rule) are included in the [_window frame_](./#frame-clause-semantics-for-window-functions), because all rows become peers of the current row.

**Notes:**

- The [`frame_start`](../../../syntax_resources/grammar_diagrams/#frame-start) clause cannot be `UNBOUNDED FOLLOWING`
- The [`frame_end`](../../../syntax_resources/grammar_diagrams/#frame-end) clause cannot be `UNBOUNDED PRECEDING`, and cannot appear before the [`frame_start`](../../../syntax_resources/grammar_diagrams/#frame-start) clause.

For example `RANGE BETWEEN CURRENT ROW AND offset PRECEDING` causes this error:

```
42P20: frame starting from current row cannot have preceding rows
```

However, `ROWS BETWEEN 7 PRECEDING AND 8 PRECEDING` _is_ allowed, even though it would never select any rows.

If the `FILTER` clause is specified, then only the input rows for which it evaluates to true are fed to the window function; other rows are discarded. As noted above, only window aggregate functions invoked using the `OVER` clause accept a `FILTER` clause.

## Examples

### First example

This shows the use of a window function with an `OVER` clause that directly specifies the [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) :

```
select
  ...
  some_window_function(...) over (partition by <column list 1> order by <column list 2>) as a1,
  ...
from ...
```

Notice that the syntax spot occupied by _"some_window_function"_ may be occupied only by a window function or an aggregate function. See the section [Informal overview of function invocation using the `OVER` clause](../functionality-overview) for runnable examples of this syntax variant.

If any other kind of function, for example _"sqrt()"_, occupies this syntax spot, then it draws this specific compilation error:

```
42809: OVER specified, but sqrt is not a window function nor an aggregate function
```

And if any other expression is used at this syntax spot, then it causes this generic compilation error:

```
42601: syntax error at or near "over"
```

### Second example

This  shows the use of two window functions with `OVER` clauses that each reference the same [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) that is defined separately in a `WINDOW` clause.

```
select
  ...
  window_fn_1(...) over w as a1,
  window_fn_2(...) over w as a2,
  ...
from ...
window w as (
  partition by <column list 1>                              -- PARTITION BY clause
  order by <column list 2>                                  -- ORDER BY clause
  range between unbounded preceding and unbounded following -- frame_clause
  )
...
```

For a runnable example of this syntax variant, see [`first_value()`,`nth_value()`, `last_value()`](../function-syntax-semantics/first-value-nth-value-last-value/).

Notice that the syntax rules allow both this:

```
window_fn_1(...) over w as a1
```

and this:

```
window_fn_1(...) over (w) as a1
```

The parentheses around the window's identifier convey no meaning, Yugabyte recommends that you don't use this form because doing so will make anybody who reads your code wonder if it _does_ convey a meaning.

### Third example

This shows how a generic [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) that is defined in a `WINDOW` clause is specialized in a particular `OVER` clause that references it.
```
select
  ...
  (window_fn(...)      over w)                                                             as a1,
  (aggregate_fn_1(...) over (w range between unbounded preceding and unbounded following)) as a2,
  (aggregate_fn_1(...) over (w range between unbounded preceding and current row))         as a3,
  ...
from ...
window w as (partition by <column list 1> order by <column list 2>)
...
```

### Fourth example

This shows how the [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) specialization technique that the third example showed can be used in successively in the `WINDOW` clause.

```
select
  ...
  (window_fn(...)      over w1) as a1,
  (aggregate_fn_1(...) over w2) as a2,
  (aggregate_fn_1(...) over w3) as a3,
  ...
from ...
window
  w1 as (partition by <column list 1> order by <column list 2>),
  w2 as (w1 range between unbounded preceding and unbounded following),
  w3 as (w1 range between unbounded preceding and current row)
```

For a runnable example of this fourth syntax variant, see [Comparing the effect of `percent_rank()`, `cume_dist()`, and `ntile()` on the same input](../../window_functions/function-syntax-semantics/percent-rank-cume-dist-ntile/#comparing-the-effect-of-percent-rank-cume-dist-and-ntile-on-the-same-input).
