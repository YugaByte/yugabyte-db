---
title: Aggregate function invocation
linkTitle: Informal functionality overview
headerTitle: Informal overview of aggregate function invocation
description: This section provides an informal introduction to the invocation of aggregate functions.
menu:
  latest:
    identifier: aggregate-functions-functionality-overview
    parent: aggregate-functions
    weight: 10
isTocNested: true
showAsideToc: true
---

Aggregate functions fall into two kinds according to the syntax that you use to invoke them.

## Ordinary aggregate functions

All of the functions listed in the two tables [General-purpose aggregate functions](../function-syntax-semantics/#general-purpose-aggregate-functions) and [Statistical aggregate functions
](../function-syntax-semantics/#statistical-aggregate-functions) are of this kind. Aggregate functions of this kind can be invoked in one of two ways:

- _Either_ "ordinarily" on all the rows in a table or in connection with `GROUP BY`, when they return a single value for the set of rows.

  In this use, row ordering often doesn't matter. For example, [`avg()`](../function-syntax-semantics/avg-count-max-min-sum/#avg) has this property. Sometimes, row ordering _does_ matter. For example, the order of grouped values determines the mapping between array index and array value with [`array_agg()`](../function-syntax-semantics/array-string-jsonb-jsonb-object-agg/#array-agg).

- _Or_ as a [window function](../../window_functions/)  with `OVER`.

  In this use, where the aggregate function is evaluated for each row in the [window](../../window_functions/invocation-syntax-semantics/#the-window-definition-rule), ordering always matters.

### Ordinary invocation

First create and populate a test table:

```plpgsql
drop table if exists t cascade;
create table t(
  k     int      primary key,
  class int      not null,
  n     numeric  not null,
  s     text     not null);

insert into t(k, class, n, s)
select
  v,
  ntile(2) over (order by v),
  (7 + v*0.1),
  chr(ascii('a') + v - 1)
from generate_series(1, 10) as g(v);

select class, to_char(n, '0.99') as n, s from t order by class, n;
```

This is the result:

```
 class |   n   | s 
-------+-------+---
     1 |  7.10 | a
     1 |  7.20 | b
     1 |  7.30 | c
     1 |  7.40 | d
     1 |  7.50 | e
     2 |  7.60 | f
     2 |  7.70 | g
     2 |  7.80 | h
     2 |  7.90 | i
```

Now demonstrate the ordinary invocation of the aggregate functions  [`count()`](../function-syntax-semantics/avg-count-max-min-sum/#count) and [`avg()`](../function-syntax-semantics/avg-count-max-min-sum/#avg):

```plpgsql
select
  count(n)                 as count,
  to_char(avg(n), '0.99')  as avg
from t;
```
This is the result:

```
 count |  avg  
-------+-------
    10 |  7.55
```
Next, add a `GROUP BY` clause:

```plpgsql
select
  class,
  count(n)                 as count,
  to_char(avg(n), '0.99')  as avg
from t
group by class
order by class;
```

This is the result:

```
 class | count |  avg  
-------+-------+-------
     1 |     5 |  7.30
     2 |     5 |  7.80
```

Next demonstrate the use of the `FILTER` syntax as part of the `SELECT` list invocation syntax of `avg()` and the `ORDER BY` syntax as part of the `SELECT` list invocation syntax of [`string_agg()`](../function-syntax-semantics/array-string-jsonb-jsonb-object-agg/#string-agg):

```plpgsql
select
  count(n)                                        as count,
  to_char(avg(n) filter (where k%3 = 0), '0.99')  as avg,
  string_agg(s, '-' order by k desc)              as s
from t;
```

This is the result:

```
 count |  avg  |          s          
-------+-------+---------------------
    10 |  7.60 | j-i-h-g-f-e-d-c-b-a
```

### Invoking an ordinary aggregate function as a window function

Every ordinary aggregate function can be invoked, also, as a window function.

See also the section [Informal overview of window function invocation using the OVER clause](../..//window_functions/functionality-overview/). This section also has examples of invoking an ordinary aggregate function as a window function.

Try this:

```plpgsql
with a as (
  select
    class,
    count(n)            over w1 as count,
    avg(n)              over w2 as avg,
    string_agg(s, '-')  over w1 as s
  from t
  window
    w1 as (partition by class order by k),
    w2 as (order by k groups between 2 preceding and 2 following))
select class, count, to_char(avg, '0.99') as avg, s
from a;
```

This is the result:

```
 class | count |  avg  |     s     
-------+-------+-------+-----------
     1 |     1 |  7.20 | a
     1 |     2 |  7.25 | a-b
     1 |     3 |  7.30 | a-b-c
     1 |     4 |  7.40 | a-b-c-d
     1 |     5 |  7.50 | a-b-c-d-e
     2 |     1 |  7.60 | f
     2 |     2 |  7.70 | f-g
     2 |     3 |  7.80 | f-g-h
     2 |     4 |  7.85 | f-g-h-i
     2 |     5 |  7.90 | f-g-h-i-j
```

Notice that the effect of the omission of the [frame clause](../../window_functions/invocation-syntax-semantics/#the-frame-clause-1) in the definition of _"w1"_ for the invocation of `count()` and `string_agg()` is to ask to use the rows from the start of the window through the current row.

Notice, too, that the effect of `groups between 2 preceding and 2 following` in the definition of _"w2"_ for the invocation of `avg()` is to compute the moving average within a window of two values below and two values above the present value.

The rules for the [window_definition rule](../../window_functions/invocation-syntax-semantics/#the-window-definition-rule)—and in particular the effect of omitting the so-called [frame clause](../../window_functions/invocation-syntax-semantics/#the-frame-clause-1)—are explained in the section [Window function invocation—SQL syntax and semantics](../../window_functions/invocation-syntax-semantics/).

## Within-group aggregate functions

This kind has two sub-kinds:

- [within-group ordered-set aggregate functions](#within-group-ordered-set-aggregate-functions)

- [within-group hypothetical-set aggregate functions](#within-group-hypothetical-set-aggregate-functions)

The invocation syntax is the same for the functions in both subgroups. But the semantic proposition is different.

### Within-group ordered-set aggregate functions

There are only three aggregate functions of this sub-kind: [`mode()`](../function-syntax-semantics/mode-percentile-disc-percentile-cont/#mode), [`percentile_disc()`](../function-syntax-semantics/mode-percentile-disc-percentile-cont/#percentile-disc-percentile-cont), and [`percentile_cont()`](../function-syntax-semantics/mode-percentile-disc-percentile-cont/#percentile-disc-percentile-cont).

The `mode()` function is chosen to illustrate the "within-group ordered-set" syntax here because its meaning is the easiest of the three to understand. It simply returns the most frequent value of the ordering expression used in this syntax:

```
within group (order by <expr>)
```

If there's more than one equally-frequent value, then one of these is silently chosen arbitrarily.

First create and populate a test table. It's convenient to use the same table and population that the `mode()` section uses in the subsection [Example that uses GROUP BY](../function-syntax-semantics/mode-percentile-disc-percentile-cont/#example-that-uses-group-by). The code is copied here for your convenience. The data is contrived so that the value _"v = 37"_ occurs twice for _"class = 1"_ and so that the value _"v = 42"_ occurs twice for _"class = 2"_. Otherwise each distinct value of _"v"_ occurs just once.

```plpgsql
drop table if exists t cascade;
create table t(
  k     int  primary key,
  class int  not null,
  v     int  not null);

insert into t(k, class, v)
select
  s.v,
  1,
  case s.v between 5 and 6
    when true then 37
              else s.v
  end
from generate_series(1, 10) as s(v)
union all
select
  s.v,
  2,
  case s.v between 15 and 17
    when true then 42
              else s.v
  end
from generate_series(11, 20) as s(v);
```

Now list out the biggest three counts for each distinct value of _"v"_ for each of the two values of _"class":

```plpgsql
select 1 as class, v, count(*) "frequency"
from t
where class = 1
group by v
order by count(*) desc, v
limit 3;

select 2 as class, v, count(*) "frequency"
from t
where class = 2
group by v
order by count(*) desc, v
limit 3;
```

These are the results:

```
 class | v  | frequency 
-------+----+-----------
     1 | 37 |         2
     1 |  1 |         1
     1 |  2 |         1

 class | v  | frequency 
-------+----+-----------
     2 | 42 |         3
     2 | 11 |         1
     2 | 12 |         1
```

Here's how to invoke the `mode()` within-group ordered-set aggregate function:

```plpgsql
select
  class,
  mode() within group (order by v) as "most frequent v"
from t
group by class
order by class;
```

Here is the result:

```
 class | most frequent v 
-------+-----------------
     1 |              37
     2 |              42
```

### Within-group hypothetical-set aggregate functions

There are four functions of this sub-kind: [`rank()`](../function-syntax-semantics/rank-dense-rank-percent-rank-cume-dist/#rank), [`dense_rank()`](../function-syntax-semantics/rank-dense-rank-percent-rank-cume-dist/#dense-rank), [`percent_rank()`](../function-syntax-semantics/rank-dense-rank-percent-rank-cume-dist/#percent-rank), and [`cume_dist()`](../function-syntax-semantics/rank-dense-rank-percent-rank-cume-dist/#cume-dist). See the section [Within-group hypothetical-set aggregate functions](../function-syntax-semantics/rank-dense-rank-percent-rank-cume-dist/) for more information.

The same functions can also be invoked as window functions. That use is described here:

- [`rank()`](../../window_functions/function-syntax-semantics/row-number-rank-dense-rank/#rank)
- [`dense_rank()`](../../window_functions/function-syntax-semantics/row-number-rank-dense-rank/#dense-rank)
- [`percent_rank()`](../../window_functions/function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank)
- [`cume_dist()`](../../window_functions/function-syntax-semantics/percent-rank-cume-dist-ntile/#cume-dist)

The basic semantic definition of each function is the same in each invocation scenario. But the goals of the two invocation methods are critically different. The window function invocation method produces the value prescribed by the function's definition for each extant row. And the  within-group hypothetical-set invocation method produces the value that the row whose relevant values are specified in the invocation _would_ produce if such a row were actually (rather than hypothetically) to be inserted.

First create and populate a test table. It's convenient to use the same table and population that's used in the subsection [Semantics demonstration](../function-syntax-semantics/rank-dense-rank-percent-rank-cume-dist/#semantics-demonstration) in the "Within-group hypothetical-set aggregate functions" section. The code is copied here for your convenience.

```plpgsql
drop table if exists t cascade;
create table t(
  k      int primary key,
  class  int not null,
  score  int);

insert into t(k, class, score)
with a as (
  select s.v from generate_series(1, 10) as s(v))
values(0, 1, null::int)
union all
select
  v,
  ntile(2) over (order by v),
  case v <= 5
    when true then v*2
              else (v - 5)*2
  end
from a;

\pset null <null>
select class, score
from t
order by class, score nulls first;
```

This is the result:

```
 class | score  
-------+--------
     1 | <null>
     1 |      2
     1 |      4
     1 |      6
     1 |      8
     1 |     10
     2 |      2
     2 |      4
     2 |      6
     2 |      8
     2 |     10
```

Next, create a view defined by a `SELECT` statement that invokes the `rank()` function as a window function:

```plpgsql
create or replace view v as
select
  k,
  class,
  score,
  (rank() over (partition by class order by score nulls first)) as r
from t;
```

Visualize the results that the view defines:

```plpgsql
select class, score, r
from v
order by class, r;
```

This is the result:

```
 class | score  | r 
-------+--------+---
     1 | <null> | 1
     1 |      2 | 2
     1 |      4 | 3
     1 |      6 | 4
     1 |      8 | 5
     1 |     10 | 6
     2 |      2 | 1
     2 |      4 | 2
     2 |      6 | 3
     2 |      8 | 4
     2 |     10 | 5
```

Now, simulate the hypothetical insert of two rows, one in each class, and visualize the values that `rank()` produces for these. Do this within a transaction that you rollback.

```plpgsql
start transaction;
insert into t(k, class, score) values (21, 1, 5), (22, 2, 6);

select class, score, r
from v
where k in (21, 22)
order by class, r;

rollback;
```

This is the result:

```
 class | score | r 
-------+-------+---
     1 |     5 | 4
     2 |     6 | 3
```

Now, mimic the two hypothetical inserts. Notice that the text of the `SELECT` statement is identical for the case where _"score"_ is set to _5_ and _"class"_ is set to _1_ and the case where _"score"_ is set to _6_ and _"class"_ is set to _2_.

```plpgsql
\set score 5
\set class 1
select
  :class as class,
  :score as score,
  rank(:score) within group (order by score nulls first) as r
from t
where class = :class;

\set score 6
\set class 2
select
  :class as class,
  :score as score,
  rank(:score) within group (order by score nulls first) as r
from t
where class = :class;
```

These are the results:

```
 class | score | r 
-------+-------+---
     1 |     5 | 4

 class | score | r 
-------+-------+---
     2 |     6 | 3
```

Notice that they are the same as were seen inside the _"start transaction;... rollback;"_ code above.

Now try the two within-group hypothetical-set invocations without the restriction `where class = :class` but instead with `GROUP BY class`:

```plpgsql
\set score 5
select
  class,
  :score as score,
  rank(:score) within group (order by score nulls first) as r
from t
group by class;

\set score 6
select
  class,
  :score as score,
  rank(:score) within group (order by score nulls first) as r
from t
group by class;
```

This is the result:

```
 class | score | r 
-------+-------+---
     1 |     5 | 4
     2 |     5 | 3

 class | score | r 
-------+-------+---
     1 |     6 | 4
     2 |     6 | 3
```

Notice that values were produced, for each value in turn of the hypothetical _"score"_, for _every_ currently existing value of _"class"_. This corresponds to what would bee seen, in the simulated insert within the rolled back transaction, if each chosen value of _"score"_ were inserted once for each currently existing value of the `GROUP BY` column _"class"_.