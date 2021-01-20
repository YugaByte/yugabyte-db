---
title: Window function invocation using the OVER clause
linkTitle: Informal functionality overview
headerTitle: Informal overview of window function invocation using the OVER clause
description: This section provides an informal introduction to the invocation of window functions and aggregate functions using the OVER clause.
menu:
  latest:
    identifier: window-functions-functionality-overview
    parent: window-functions
    weight: 10
isTocNested: true
showAsideToc: true
---

A good sense of the general functionality of window functions is given by examples that use [`row_number()`](../function-syntax-semantics/row-number-rank-dense-rank/#row-number), [`nth_value()`](../function-syntax-semantics/first-value-nth-value-last-value/#nth-value), [`last_value()`](../function-syntax-semantics/first-value-nth-value-last-value/#last-value), [`lag()`](../function-syntax-semantics/lag-lead/#lag), and [`lead()`](../function-syntax-semantics/lag-lead/#lead). 

Aggregate functions can be invoked with the `OVER` clause. Examples are given using `avg()` and `sum()`.

These examples are sufficient to give a general sense of the following notions:

- how window functions are invoked, and their general semantics
- the three clauses of the [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) : the `PARTITION BY` clause, the window `ORDER BY` clause, and the [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause)
- how an aggregate function gains different useful functionality when it's invoked using an `OVER` clause rather than (as is probably more common) in conjunction with the regular `GROUP BY` clause.

{{< note title=" " >}}

If you haven't yet installed the tables that the code examples use, then go to the section [The data sets used by the code examples](../function-syntax-semantics/data-sets/).

{{< /note >}}

## Using row_number() in the simplest way

The [`row_number()`](../function-syntax-semantics/row-number-rank-dense-rank/#row-number) window function is the simplest among the set of eleven such functions that YSQL supports. Briefly, this function assigns an ordinal number, starting at _1_, to the rows within the specified [_window_](../invocation-syntax-semantics/#the-window-definition-rule) according to the specified ordering rule. Here is the most basic example.

```plpgsql
select
  k,
  row_number() over(order by k desc) as r
from t1
order by k asc;
```

The syntax and semantics of the `ORDER BY` clause, within the parentheses of the `OVER` clause, are identical to what you're used to when an `ORDER BY` clause is used after the `FROM` clause in a subquery. The `DESC` keyword is used in this example to emphasize this point. It says that the values returned by [`row_number()`](../function-syntax-semantics/row-number-rank-dense-rank/#row-number) are to be assigned in the order corresponding to sorting the values of _"k"_ in descending order—and it specifies nothing else. Here is the result:

```
 k  | r  
----+----
  1 | 25
  2 | 24
  3 | 23
  4 | 22
  5 | 21
  ...
 21 |  5
 22 |  4
 23 |  3
 24 |  2
 25 |  1
```

The output lines for values of _"r"_ between _6_ and _20_ were manually removed to reduce the clutter.

Because the `OVER` clause doesn't specify a `PARTITION BY` clause, the so-called [_window_](../invocation-syntax-semantics/#the-window-definition-rule) that [`row_number()`](../function-syntax-semantics/row-number-rank-dense-rank/#row-number) operates on coincides with all of the rows in table _"t1"_. 

The next example emphasizes the point that a window function is often used in a subquery which, like any other subquery, is used to define a `WITH` clause view to allow further logic to be applied—in this case, a `WHERE` cause restriction on the values returned by [`row_number()`](../function-syntax-semantics/row-number-rank-dense-rank/#row-number) (and, of course, a final query-level `ORDER BY` rule).

```plpgsql
with v as (
  select
    k,
    row_number() over(order by k desc) as r
  from t1)
select
  k,
  r
from v
where (r between 1 and 5) or (r between 21 and 25)
order by r asc;
```

This is the result:

```
 k  | r  
----+----
 25 |  1
 24 |  2
 23 |  3
 22 |  4
 21 |  5
  5 | 21
  4 | 22
  3 | 23
  2 | 24
  1 | 25
```

## Showing the importance of the window ORDER BY clause

Here is a counter example. Notice that the [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) doesn't specify a window `ORDER BY` clause.

```plpgsql
with a as (
  select
    -- The use of the bare OVER() here brings meaningless results.
    row_number() over () as r,
    class,
    k
  from t1)
select
  r,
  class,
  k,
  case k=r
    when true then 'true'
    else           ''
  end as chk
from a
order by r;
```

To see the most dramatic effect of the unpredictability of the result set, save the code from [table t1](../function-syntax-semantics/data-sets/table-t1/) into a file called, say, _"unpredictable.sql"_. Then copy the SQL statement, above, at the end of this file and invoke it time and again in `ysqlsh`. Here is a typical result:

```
 r  | class | k  | chk  
----+-------+----+------
  1 |     5 | 23 | 
  2 |     5 | 25 | 
  3 |     2 |  9 | 
  4 |     1 |  4 | true
  5 |     3 | 11 | 
  6 |     1 |  1 | 
  7 |     3 | 13 | 
  8 |     4 | 16 | 
  9 |     1 |  2 | 
 10 |     2 |  7 | 
 11 |     1 |  3 | 
 12 |     4 | 18 | 
 13 |     3 | 15 | 
 14 |     5 | 21 | 
 15 |     3 | 14 | 
 16 |     3 | 12 | 
 17 |     4 | 17 | true
 18 |     1 |  5 | 
 19 |     2 | 10 | 
 20 |     4 | 20 | true
 21 |     5 | 24 | 
 22 |     5 | 22 | true
 23 |     2 |  6 | 
 24 |     2 |  8 | 
 25 |     4 | 19 | 
```

Sometimes, you'll see that, by chance, not a single output row is marked _"true"_. Sometimes, you'll see that a few are so marked.

## Using row_number() with "PARTITION BY"

This example adds a `PARTITION BY` clause to the window `ORDER BY` clause in the [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) . It selects and orders by _"v"_ rather than _"k"_ because this has `NULL`s and demonstrates the within-[_window_](../invocation-syntax-semantics/#the-window-definition-rule) effect of `NULLS FIRST`. The [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) is moved to a dedicated `WINDOW` clause that names it so that the `OVER` clause can simply reference the definition that it needs. This might seem only to add verbosity in this example. But using a dedicated `WINDOW` clause reduces verbosity when invocations of several different window functions in the same subquery use the same [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition).

```plpgsql
\pset null '??'

with a as (
  select
    class,
    v,
    row_number() over w as r
  from t1
  window w as (partition by class order by v desc nulls first))
select
  class,
  v,
  r
from a
where class in (2, 4)
order by class, r;
```

This is the result:

```
 class | v  | r 
-------+----+---
     2 | ?? | 1
     2 |  9 | 2
     2 |  8 | 3
     2 |  7 | 4
     2 |  6 | 5
     4 | ?? | 1
     4 | 19 | 2
     4 | 18 | 3
     4 | 17 | 4
     4 | 16 | 5
```

## Using nth_value() and last_value() to return the whole row

If you want the output value for any of `first_value()`, `last_value()`, `nth_value()`, `lag()`, or `lead()` to include more than one column, then you must list them in a _"row"_ type constructor. This example uses [`nth_value()`](../function-syntax-semantics/first-value-nth-value-last-value/#nth-value). This accesses the _Nth_ row within the ordered set that each [_window_](../invocation-syntax-semantics/#the-window-definition-rule) defines. It picks out the third row. The restriction _"class in (3, 5)"_ cuts down the result set to make it easier to read.

```plpgsql
drop type if exists rt cascade;
create type rt as (class int, k int, v int);

select
  class,
  nth_value((class, k, v)::rt, 3) over w as nv
from t1
where class in (3, 5)
window w as (
  partition by class
  order by k
  range between unbounded preceding and unbounded following
  )
order by class;
```

It produces this result:

```
 class |    nv     
-------+-----------
     3 | (3,13,13)
     3 | (3,13,13)
     3 | (3,13,13)
     3 | (3,13,13)
     3 | (3,13,13)
     5 | (5,23,23)
     5 | (5,23,23)
     5 | (5,23,23)
     5 | (5,23,23)
     5 | (5,23,23)
```

Each of `first_value()`, `last_value()`, and `nth_value()`, as their names suggest, produces the same output for each row of a [_window_](../invocation-syntax-semantics/#the-window-definition-rule). It would be natural, therefore, to use the query above in a `WITH` clause whose final `SELECT` picks out the individual columns from the record and adds a `GROUP BY` clause, thus:

```plpgsql
drop type if exists rt cascade;
create type rt as (class int, k int, v int);

\pset null '??'
with a as (
  select
    last_value((class, k, v)::rt) over w as lv
  from t1
  window w as (
    partition by class
    order by k
    range between unbounded preceding and unbounded following))
select
  (lv).class,
  (lv).k,
  (lv).v
from a
group by class, k, v
order by class;
```

This example uses [`last_value()`](../function-syntax-semantics/first-value-nth-value-last-value/#last-value) because the data set has different values for _"k"_ and _"v"_ for the last row in each [_window_](../invocation-syntax-semantics/#the-window-definition-rule). This is the result:

```
 class | k  | v  
-------+----+----
     1 |  5 | ??
     2 | 10 | ??
     3 | 15 | ??
     4 | 20 | ??
     5 | 25 | ??
```

## Using lag() and lead() to compute a moving average

The aim is to compute the moving average for each day within the window, where this is feasible, over the last-but one day, the last day, the current day, the next day, and the next-but-one day.

Notice that the following section uses the aggregate function `avg()` to produce the same result, and it shows the advantages of that approach over using the window functions [`lag()`](../function-syntax-semantics/lag-lead/#lag) and [`lead()`](../function-syntax-semantics/lag-lead/#lead). There are many other cases where `lag()` and/or  `lead()`are needed and where `avg()` is of no use. The present use case was chosen here because it shows very clearly what `lag()` and `lead()` do and, especially, because it allows the demonstration of invoking an aggregate function with an `OVER` clause.

The query is specifically written to meet the exact requirements. It would need to be manually re-written to base the moving average on a bigger, or smaller, range of days. Notice that the same [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) , _"w"_, is used as the argument for each of the four uses of the `OVER` clause. This is where using a separate `WINDOW` clause delivers its intended benefit.

The statement of requirement implies that the computation is not feasible for the first two and the last two days in the window. Under these circumstances, `lag()` and `lead()`, return `NULL`—or, it you prefer, a default value that you supply using an optional third parameter. See the dedicated section on [`lag()` and `lead()`](../function-syntax-semantics/lag-lead/) for details.

```plpgsql
with v as (
  select
    day,
    lag (price::numeric, 2) over w as lag_2,
    lag (price::numeric, 1) over w as lag_1,
    price::numeric,
    lead(price::numeric, 1) over w as lead_1,
    lead(price::numeric, 2) over w as lead_2
  from t3
  window w as (order by day))
select
  to_char(day, 'Dy DD-Mon') as "Day",
  ((lag_2 + lag_1 + price + lead_1 + lead_2)/5.0)::money as moving_avg
from v
where (lag_2 is not null) and (lead_2 is not null)
order by day;
```

This is the result:

```
    Day     | moving_avg 
------------+------------
 Wed 17-Sep |     $18.98
 Thu 18-Sep |     $19.13
 Fri 19-Sep |     $19.27
 Mon 22-Sep |     $19.64
 Tue 23-Sep |     $19.99
 Wed 24-Sep |     $20.10
 Thu 25-Sep |     $19.90
 Fri 26-Sep |     $19.62
 Mon 29-Sep |     $19.60
 Tue 30-Sep |     $19.41
 Wed 01-Oct |     $19.18
 Thu 02-Oct |     $19.08
 Fri 03-Oct |     $18.78
 Mon 06-Oct |     $18.19
 Tue 07-Oct |     $17.53
 Wed 08-Oct |     $16.97
 Thu 09-Oct |     $17.08
 Fri 10-Oct |     $17.26
 Mon 13-Oct |     $17.08
 Tue 14-Oct |     $17.23
 Wed 15-Oct |     $17.30
```

## Using the aggregate function avg() to compute a moving average

This solution takes advantage of this [`window_definition`](../../../syntax_resources/grammar_diagrams/#window-definition) to determine the rows that `avg()` uses:

```
order by day groups between $1 preceding and $1 following
```

Here, the statement is first prepared and then executed to emphasize the fact that a single formulation of the statement text works for any arbitrary range of days around the current row. The section [Window function invocation—SQL syntax and semantics](../invocation-syntax-semantics/) explains the full power of expression brought by the `OVER` clause.

Notice that this approach uses the value returned by [`row_number()`](../function-syntax-semantics/row-number-rank-dense-rank/#row-number), using an `OVER` clause that does no more than order the rows, to exclude the meaningless first _N_ and last _N_ averages, where _N_ is the same parameterized value that _"groups between N preceding and N following"_ uses. These rows, if not excluded, would simply show the averages over the rows that allow access. You probably don't want to see those answers.

```plpgsql
prepare stmt(int) as
with v as (
  select
    day,
    avg(price::numeric) over w1 as a,
    row_number()        over w2 as r
  from t3
  window
    w1 as (order by day groups between $1 preceding and $1 following),
    w2 as (order by day))
select
  to_char(day, 'Dy DD-Mon') as "Day",
  a::money as moving_avg
from v
where r between ($1 + 1) and (select (count(*) - $1) from v)
order by day;

execute stmt(2);
```

The result is identical to that produced by the `lag()`/`lead()` approach. Try repeating the `EXECUTE` statement with a few different actual arguments. The bigger it gets, the fewer result rows you see, and the closer the values of the moving average get to each other.

## Using the aggregate function sum() with the OVER clause

This example shows a different spelling of the [`frame_clause`](../../../syntax_resources/grammar_diagrams/#frame-clause):

```
range between unbounded preceding and current row
```

so that the average includes, for each row, the row itself and only the rows that precede it in the sort order.

```plpgsql
with v as (
  select
    class,
    k,
    sum(k) over w as s
  from t1
  window w as (
    partition by class
    order by k
    range between unbounded preceding and current row))
select
  class,
  k,
  s
from v
where class in (2, 4)
order by class, k;
```

This is the result:

```
 class | k  | s  
-------+----+----
     2 |  6 |  6
     2 |  7 | 13
     2 |  8 | 21
     2 |  9 | 30
     2 | 10 | 40
     4 | 16 | 16
     4 | 17 | 33
     4 | 18 | 51
     4 | 19 | 70
     4 | 20 | 90
```
