---
title: Aggregate function syntax and semantics
linkTitle: Invocation syntax and semantics
headerTitle: Aggregate function invocation—SQL syntax and semantics
description: This section specifies the syntax and semantics of aggregate function invocation.
menu:
  latest:
    identifier: aggregate-functions-invocation-syntax-semantics
    parent: aggregate-functions
    weight: 20
isTocNested: true
showAsideToc: true
---

## Syntax

### Reproduced from the SELECT statement section

The following six diagrams, [`select_start`](../../../syntax_resources/grammar_diagrams/#select-start), [`ordinary_aggregate_fn_invocation`](../../../syntax_resources/grammar_diagrams/#ordinary-aggregate-fn-invocation), [`within_group_aggregate_fn_invocation`](../../../syntax_resources/grammar_diagrams/#within-group-aggregate-fn-invocation), [`group_by_clause`](../../../syntax_resources/grammar_diagrams/#group-by-clause), [`grouping_element`](../../../syntax_resources/grammar_diagrams/#grouping-element), and [`having_clause`](../../../syntax_resources/grammar_diagrams/#having-clause) are reproduced from the section that describes the [`SELECT` statement](../../../the-sql-language/statements/dml_select/).

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
    {{% includeMarkdown "../../syntax_resources/exprs/aggregate_functions/select_start,ordinary_aggregate_fn_invocation,within_group_aggregate_fn_invocation.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/exprs/aggregate_functions/select_start,ordinary_aggregate_fn_invocation,within_group_aggregate_fn_invocation.diagram.md" /%}}
  </div>
</div>
These rules govern the invocation of aggregate functions as `SELECT` list items.

The aggregate functions listed in the sections [General-purpose aggregate functions](../function-syntax-semantics/#general-purpose-aggregate-functions) and [Statistical aggregate functions](../function-syntax-semantics/#statistical-aggregate-functions) are governed by the `ordinary_aggregate_fn_invocation` rule.  These functions may also be invoked as window functions. See the account of the `fn_over_window` rule, and everything else that qualifies this, in the section [Window function invocation—SQL syntax and semantics](../../window_functions/invocation-syntax-semantics/).

The aggregate functions listed in the sections [Within-group ordered-set aggregate functions](../function-syntax-semantics/#within-group-ordered-set-aggregate-functions) and [Within-group hypothetical-set aggregate functions](../function-syntax-semantics/#within-group-hypothetical-set-aggregate-functions) are governed by the `within_group_aggregate_fn_invocation` rule.  "Within-group ordered-set" aggregate functions may _not_ be invoked as window functions. But "within-group hypothetical-set" aggregate functions _may_ be invoked as window functions. The reasons for this difference are explained in the two relevant dedicated sections.

When aggregate functions are invoked using the syntax specified by either the `ordinary_aggregate_fn_invocation` rule or the `within_group_aggregate_fn_invocation` rule, users very often determine the result set with the `GROUP BY` clause.

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
    {{% includeMarkdown "../../syntax_resources/exprs/aggregate_functions/group_by_clause,grouping_element.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/exprs/aggregate_functions/group_by_clause,grouping_element.diagram.md" /%}}
  </div>
</div>

The result set may be restricted by the `HAVING` clause:

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
    {{% includeMarkdown "../../syntax_resources/exprs/aggregate_functions/having_clause.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/exprs/aggregate_functions/having_clause.diagram.md" /%}}
  </div>
</div>

## Semantics

### The ordinary_aggregate_fn_invocation rule

This syntax rule governs the invocation of the aggregate functions that are listed in the [General-purpose aggregate functions](../function-syntax-semantics/#general-purpose-aggregate-functions) and the [Statistical aggregate functions](../function-syntax-semantics/#statistical-aggregate-functions) sections. Notice that (possibly to your surprise) the optional `ORDER BY` clause is used _within_ the parentheses that surround the arguments with which the function is invoked and that there is no comma after the final argument and this clause. Here is an example:

```plpgsql
drop table if exists t cascade;
create table t(
  k     int   primary key,
  class int   not null,
  v     text  not null);

insert into t(k, class, v)
select
  (1 + s.v),
  case (s.v) < 3
    when true then 1
              else 2
  end,
  chr(97 + s.v)
from generate_series(0, 5) as s(v);

select
  class,
  array_agg(v            order by k desc) as "array_agg(v)",
  string_agg(v, ' ~ '    order by k desc) as "string_agg(v)",
  jsonb_agg(v            order by v desc) as "jsonb_agg",
  jsonb_object_agg(v, k  order by v desc) as "jsonb_object_agg(v, k)"
from t
group by class
order by class;
```
It produces this result:

```
 class | array_agg(v) | string_agg(v) |    jsonb_agg    |  jsonb_object_agg(v, k)  
-------+--------------+---------------+-----------------+--------------------------
     1 | {c,b,a}      | c ~ b ~ a     | ["c", "b", "a"] | {"a": 1, "b": 2, "c": 3}
     2 | {f,e,d}      | f ~ e ~ d     | ["f", "e", "d"] | {"d": 4, "e": 5, "f": 6}
```

This is a simplified version of the example shown in the [`GROUP BY` syntax](../function-syntax-semantics/array-string-jsonb-jsonb-object-agg/#group-by-syntax) section within the [`array_agg()`, `string_agg()`, `jsonb_agg()`, `jsonb_object_agg()`](../function-syntax-semantics/array-string-jsonb-jsonb-object-agg/) section. These three functions:

- [`array_agg()`](../function-syntax-semantics/array-string-jsonb-jsonb-object-agg/#array-agg), [`string_agg()`](../function-syntax-semantics/array-string-jsonb-jsonb-object-agg/#string-agg), [`jsonb_agg()`](../function-syntax-semantics/array-string-jsonb-jsonb-object-agg/#jsonb-agg)

are sensitive to the effect of the order of aggregation of the individual values. This is because they produce lists. However, [`jsonb_object_agg()`](../function-syntax-semantics/array-string-jsonb-jsonb-object-agg/#jsonb-object-agg) is _not_ sensitive to the order because the key-value pairs in a JSON object are defined to have no order. And neither is any other aggregate function among those that are governed by the `ordinary_aggregate_fn_invocation` sensitive to ordering.

The [`string_agg()`](../function-syntax-semantics/array-string-jsonb-jsonb-object-agg/#string-agg) function conveniently illustrates the effect of the `FILTER` clause:

```plpgsql
select
  string_agg(v, ' ~ ' order by k     ) filter (where v <> 'f') as "string_agg(v) without f",
  string_agg(v, ' ~ ' order by k desc) filter (where v <> 'a') as "string_agg(v) without a"
from t;
```
This is the result:

```
 string_agg(v) without f | string_agg(v) without a 
-------------------------+-------------------------
 a ~ b ~ c ~ d ~ e       | f ~ e ~ d ~ c ~ b
```

### The within_group_aggregate_fn_invocation rule

This syntax rule governs the invocation of the aggregate functions that are listed in the [Within-group ordered-set aggregate functions](../function-syntax-semantics/#within-group-ordered-set-aggregate-functions) section and the [Within-group hypothetical-set aggregate functions](../function-syntax-semantics/#within-group-hypothetical-set-aggregate-functions) section.

The [`mode()`](../function-syntax-semantics/mode-percentile-disc-percentile-cont/#mode) function is a "within-group ordered-set" aggregate function. Here's a simple example:

```plpgsql
drop table if exists t cascade;
create table t(
  k     int  primary key,
  class int not null,
  v     text);

insert into t(k, class, v)
select
  g.v,
  ntile(2) over(order by v),
  chr(ascii('a') -1 + g.v)
from generate_series(1, 10) as g(v)
union all
values
  (11, 1, 'e'),
  (12, 2, 'f'),
  (13, 2, null),
  (14, 2, null),
  (15, 2, null);

\pset null <null>
select k, class, v from t order by class, v nulls last, k;
```

This is the result:

```
 k  | class |   v    
----+-------+--------
  1 |     1 | a
  2 |     1 | b
  3 |     1 | c
  4 |     1 | d
  5 |     1 | e
 11 |     1 | e
  6 |     2 | f
 12 |     2 | f
  7 |     2 | g
  8 |     2 | h
  9 |     2 | i
 10 |     2 | j
 13 |     2 | <null>
 14 |     2 | <null>
 15 |     2 | <null>
```

Now try this:

```plpgsql
select
  class,
  mode() within group (order by k desc) as "k mode",
  mode() within group (order by v     ) as "v mode"
from t
group by class
order by class;
```

This is the result:

```
 class | k mode | v mode 
-------+--------+--------
     1 |     11 | e
     2 |     15 | f
```

Because _"k"_ happens to be unique, the modal value is chosen arbitrarily from the set of candidate values. It might appear that the `ORBER BY` clause determines which value is chosen. Don't rely on this—it's an undocumented effect of the implementation and might change at some future release boundary.

Notice that the expression for which the modal value for each value of _"class"_, as the `GROUP BY` clause requests,  is specified not as the argument of the [`mode()`](../function-syntax-semantics/mode-percentile-disc-percentile-cont/#mode) function but, rather, as the argument of the invocation's `ORDER BY` clause. This explains why the `within_group_aggregate_fn_invocation` rule specifies that `ORDER BY` is mandatory. If you execute the `\df mode` metacommand in `ysqlsh`, you'll see that both the argument data type and the result data type is `anyelement`. In other words, the argument of the `ORDER BY` clause in the invocation of the [`mode()`](../function-syntax-semantics/mode-percentile-disc-percentile-cont/#mode) aggregate function must be just a single scalar expression. Notice that this is more restrictive than the general case for the `ORDER BY` clause that you use at top level in a subquery or within the window definition for the `OVER` clause that you use to invoke a window function.

The expression need not correspond just to a bare column, as this example shows:

```plpgsql
select
  mode() within group (order by v||'x')                                              as "expr-1 mode",
  mode() within group (order by (case v is null when true then '<null>' else v end)) as "expr-2 mode"
from t;
```

This is the result:

```
 expr-1 mode | expr-2 mode 
-------------+-------------
 ex          | <null>
```

The parameterization story for the other two "within-group ordered-set" aggregate functions, [`percentile_disc()`](../function-syntax-semantics/mode-percentile-disc-percentile-cont/#percentile-disc-percentile-cont) and [`percentile_cont()`](../function-syntax-semantics/mode-percentile-disc-percentile-cont/#qqq), is more subtle. Each has two overloads. One takes a scalar, and the other takes an array. These arguments specify _how_ the functions should determine their result. The expression, for which the result is produced, is specified as the argument of the `ORDER BY` clause.

The syntax rules for the four [within-group hypothetical-set](../function-syntax-semantics/rank-dense-rank-percent-rank-cume-dist/) aggregate functions, [`rank()`](../function-syntax-semantics/rank-dense-rank-percent-rank-cume-dist/#rank), [`dense_rank()`](../function-syntax-semantics/rank-dense-rank-percent-rank-cume-dist/#dense-rank), [`percent_rank()`](../function-syntax-semantics/rank-dense-rank-percent-rank-cume-dist/#percent-rank), and [`cume_dist()`](../function-syntax-semantics/rank-dense-rank-percent-rank-cume-dist/#cume-dist), are, as stated, the same as for the the [within-group ordered-set](../function-syntax-semantics/mode-percentile-disc-percentile-cont/) aggregate functions. But the semantics are importantly different—and this difference is reflected in how the invocations are parameterized. This is best understood by reading the accounts of the four functions and the general introduction to the section that describes these. Briefly, the argument to the function specifies the value that is to be hypothetically inserted. And the `ORDER BY` argument specifies the expression to which that value will be assigned as a result of the hypothetical insert.

### The GROUP BY clause

The `group_by_clause` rule, together with the `grouping_element` rule, show that the `GROUP BY` clause can be composed as a comma-separated list of an unlimited number of terms, each of which can be chosen from a list of five kinds of element. Moreover, the `GROUPING SETS` alternative itself takes a comma-separated list of an unlimited number of terms, each of which can be chosen from the same list of five kinds of element. Further, this freedom can be exercised recursively. Here's an exotic example to illustrate this freedom of composition:

```plpgsql
drop table if exists t cascade;
create table t(
  k  int primary key,
  g1 int not null,
  g2 int not null,
  g3 int not null,
  g4 int not null,
  v  int not null);

insert into t(k, g1, g2, g3, g4, v)
select
  g.v,
  g.v%2,
  g.v%4,
  g.v%8,
  g.v%16,
  g.v*100
from generate_series(1, 80) as g(v);

select count(*) as "number of resulting rows" from (
  select g1, g2, g3, g4, avg(v)
  from t
  group by (), g1, (g2, g3), rollup (g1, g2), cube (g3, g4), grouping sets (g1, g2, (), rollup (g1, g3), cube (g2, g4))
  order by g1 nulls last, g2 nulls last)
as a;
```

This is the result:

```
 number of resulting rows 
--------------------------
                     1536
```

You can, of course, remove the surrounding `select count(*)... from... as a;` from this:

```
select count(*) as "number of resulting rows" from (
  select ...)
as a;
```

and look at all _1,536_ resulting rows. But it's very unlikely that you'll be able to discern any meaning from what you see. Here are two more legal examples whose meaning is obscured by the way they're written:

```plpgsql
select avg(v)
from t
group by ();
```

and

```plpgsql
select g1, avg(v)
from t
group by (), g1;
```

The meaning of each of the last three constructs of the five that the `grouping_element` rule allows is explained in the section [Using the `GROUPING SETS`, `ROLLUP`, and `CUBE` syntax for aggregate function invocation](../grouping-sets-rollup-cube/).

The second construct is the familiar bare list of `GROUP BY` expressions. This may be surrounded by parentheses, and arbitrary sequences of expressions may themselves be surrounded by arbitrary numbers of arbitrarily deeply nested parentheses pairs. However, doing this brings no meaning—just as it brings no meaning in this contrived, but legal, example:

```plpgsql
select (((((1 + 2)))) + (((((3 + (4))))))) as x;
```
It produces the answer _10_.

The first construct, the empty `()` pair has no semantic value except when it's used within, for example, the `ROLLUP` argument.

The overwhelmingly common way to take advantage of the freedoms that the `grouping_element` rule allows is to use exactly one of the last four constructs and to take advantage of the empty `()` pair in that context.

### The HAVING clause

The `HAVING` clause is functionally equivalent to the `WHERE` clause. However, it is legal only in a subquery that has a `GROUP BY` clause, and it must be placed after the `GROUP BY`. First, create and populate a test table:

```plpgsql
drop table if exists t cascade;
create table t(
  k     int      primary key,
  class int      not null,
  v     numeric);

insert into t(k, class, v)
select
  (1 + s.v),
  case (s.v) < 5
    when true then 1
              else 2
  end,
  case (s.v) <> 4
    when true then (100.0 + s.v)::numeric
              else null
  end
from generate_series(0, 9) as s(v);

\pset null <null>
select k, class, v from t order by k;
```

This is the result:

```
 k  | class |   v    
----+-------+--------
  1 |     1 |    100
  2 |     1 |    101
  3 |     1 |    102
  4 |     1 |    103
  5 |     1 | <null>
  6 |     2 |    105
  7 |     2 |    106
  8 |     2 |    107
  9 |     2 |    108
 10 |     2 |    109
```

Now try this counter-example:

```plpgsql
select v from t having v >= 105;
```

It causes this error:

```
42803: column "t.v" must appear in the GROUP BY clause or be used in an aggregate function
```

The meaning is "...must be used in an expression in the `GROUP BY` clause or be used in an expression in an aggregate function invocation".

Here is an example of the legal use of the `HAVING` clause:

```plpgsql
select class, count(v)
from t
group by class
having count(v) > 4
order by class;
```

This is the result:

```
 class | count 
-------+-------
     2 |     5
```

This illustrates the use case that motivates the `HAVING` clause: you want to restrict the results using a predicate that references an aggregate function. Try this counter-example:

```plpgsql
select class, count(v)
from t
where count(v) > 4
group by class
order by class;
```

It causes this error:

```
42803: aggregate functions are not allowed in WHERE
```

(The error code _42803_ maps to the exception name `grouping_error` in PL/pgSQL.)

In contrast, this is legal:

```plpgsql
select class, count(v)
from t
where class = 1
group by class
order by class;
```

The `WHERE` clause restricts the set on which aggregate functions are evaluated. And the `HAVING` clause restricts the result set _after_ aggregation. This informs you that a subquery that uses a `HAVING` clause legally can always be re-written to use a `WHERE` clause, albeit at the cost of increased verbosity, to restrict the result set of a subquery defined in a `WITH` clause, like this:

```plpgsql
with a as (
  select class, count(v)
  from t
  group by class)
select class, count
from a
where count > 4  
order by class;
```



