---
title: rank(), dense_rank(), percent_rank(), cume_dist()
linkTitle: rank(), dense_rank(), percent_rank(), cume_dist()
headerTitle: Within-group hypothetical-set aggregate functions
description: Describes the within-group hypothetical-set functionality of the rank(), dense_rank(), percent_rank(), cume_dist() YSQL aggregate functions
menu:
  latest:
    identifier: rank-dense-rank-percent-rank-cume-dist
    parent: aggregate-function-syntax-semantics
    weight: 100
isTocNested: true
showAsideToc: true
---

This section describes the uses of [`rank()`](./#rank), [`dense_rank()`](./#dense-rank), [`percent_rank()`](./#percent-rank), and [`cume_dist()`](./#cume-dist) as "within-group hypothetical-set" aggregate functions.

The same functions can also be invoked as window functions. That use is described here:

- [`rank()`](../../../window_functions/function-syntax-semantics/row-number-rank-dense-rank/#rank)
- [`dense_rank()`](../../../window_functions/function-syntax-semantics/row-number-rank-dense-rank/#dense-rank)
- [`percent_rank()`](../../../window_functions/function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank)
- [`cume_dist()`](../../../window_functions/function-syntax-semantics/percent-rank-cume-dist-ntile/#cume-dist)

The basic semantic definition of each function is the same in each invocation scenario. But the goals of the two invocation methods are critically different.

**Syntax example:** Suppose that table _"t"_ can be populated like this:

```
insert into t(g1,... gN, v1,... vN) values (:a,... :b, :c,... :d);
```

The columns with names like _"gN"_ represent the grouping columns and the columns with names like _"vN"_ represent the columns whose values will be used to invoke the function. In the "within-group hypothetical-set" use, the function _"f()"_ would be invoked like this:

```
select
  g1,... gN,
  f(:v1,... :vN) within group (order by v1,... vN)
from t
group by g1,... gN;
```
For each of these hypothetical-set aggregates, the list of actual arguments specified for the function must match the number and types of the column expressions specified following `ORDER BY`.

Consider this counter-example:

```
select
  g1
  f(:v1,... :vN) within group (order by v1,... vN)
from t;
```

Notice that _"g1"_ is not mentioned in a group by clause. It causes this error:

```
42803: column "t.g1" must appear in the GROUP BY clause or be used in an aggregate function
```

**Generic purpose:** Suppose that _"f()"_ is invoked as a window function like this:

```
select
  g1,... gN, v1,... vN,
  f() over(partition by g1,... gN order by v1,... vN)
from t;
```

Imagine, _hypothetically_, that, now, a row is inserted with a particular set of values for _"(g1,... gN, v1,... vN)"_, that the same `SELECT` is repeated, and that the value returned by _"f()"_ for the new row is  _"x"_. The data type of _"x"_ is function-specific. It's `bigint` for `rank()` and `dense_rank()`. And it's `double precision` for `percent_rank()` and `cume_dist()`. This thought experiment can be trivially conducted in actuality simply by starting a transaction, inserting the new row, running the query, noting the result for the new row, and then rolling back.

Suppose that, instead of doing the _hypothetical_ insert, the function is invoked using the "within-group hypothetical-set" syntax shown above using the values for _"(v1,... vN)"_ that were _hypothetically_ used for the thought experiment. The result will be, for the _hypothesized_ values for  _"(g1,... gN)"_, the same as would have been seen if the _"start transaction;... rollback;"_ approach had been used. But, beyond this, values will be produced for each possible combination of values for  _"(g1,... gN)"_ that occur in the table at the moment that the `SELECT` statement is executed. (The expressions _"g1,... gN"_ are those that are mentioned in the `GROUP BY` clause.) See the [Semantics demonstration](./#semantics-demonstration) section.

Unlike most built-in aggregate functions, these aggregate functions are not strict—that is they do not filter out rows containing `NULL`s. Rather, `NULL`s sort according to the rule specified in the `ORDER BY` clause.

## rank()

**Signature:**

```
input value:       VARIADIC "any" ORDER BY VARIADIC "any"
                   
return value:      bigint
```

**Purpose:** Returns the integer ordinal rank of each row according to the emergent order that the window `ORDER BY` clause specifies. The series of values starts with _1_ but, when the [_window_](../../../window_functions/invocation-syntax-semantics/#the-window-definition-rule) contains ties, the series is not dense.

See the account of [`rank()`](../../../window_functions/function-syntax-semantics/row-number-rank-dense-rank/#rank) in the [Window functions](../../../window_functions/) section for more information.

## dense_rank()

**Signature:**

```
input value:       VARIADIC "any" ORDER BY VARIADIC "any"
                   
return value:      bigint
```

**Purpose:** Returns the integer ordinal rank of the distinct value of each row according to what the window `ORDER BY` clause specifies. The series of values starts with _1_ and, even when the [_window_](../../../window_functions/invocation-syntax-semantics/#the-window-definition-rule) contains ties, the series is dense.

See the account of [`dense_rank()`](../../../window_functions/function-syntax-semantics/row-number-rank-dense-rank/#dense-rank) in the [Window functions](../../../window_functions/) section for more information.

## percent_rank()

**Signature:**

```
input value:       VARIADIC "any" ORDER BY VARIADIC "any"
                   
return value:      double precision
```

**Purpose:** Returns the percentile rank of each row within the [_window_](../../../window_functions/invocation-syntax-semantics/#the-window-definition-rule), with respect to the argument of the [`window_definition`](../../../../syntax_resources/grammar_diagrams/#window-definition)'s window `ORDER BY` clause. The value _p_ returned by `percent_rank()` is a number in the range _0 <= p <= 1_. It is calculated like this:

```
percentile_rank = (rank - 1) / ("no. of rows in window" - 1)
```

See the account of [`percent_rank()`](../../../window_functions/function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank) in the [Window functions](../../../window_functions/) section for more information.

## cume_dist()

```
input value:       VARIADIC "any" ORDER BY VARIADIC "any"
                   
return value:      double precision
```

**Purpose:** Returns a value that represents the number of rows with values less than or equal to the current row’s value divided by the total number of rows—in other words, the relative position of a value in a set of values. The graph of all values of `cume_dist()` within the [_window_](../../../window_functions/invocation-syntax-semantics/#the-window-definition-rule) is known as the cumulative distribution of the argument of the [`window_definition`](../../../../syntax_resources/grammar_diagrams/#window-definition)'s window `ORDER BY` clause. The value _c_ returned by `cume_dist()` is a number in the range _0 <= c <= 1_. It is calculated like this:

```
cume_dist() =
  "no of rows with a value <= the current row's value" /
  "no. of rows in window"
```

See the account of [`cume_dist()`](../../../window_functions/function-syntax-semantics/percent-rank-cume-dist-ntile/#cume-dist) in the [Window functions](../../../window_functions/) section for more information.

## Semantics demonstration

First, create and populate the test table:

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

Next, create a view defined by a `SELECT` statement that invokes the four functions of interest as window functions:

```plpgsql
create or replace view v as
select
  k,
  class,
  score,
  (rank()         over w) as r,
  (dense_rank()   over w) as dr,
  (percent_rank() over w) as pr,
  (cume_dist()    over w) as cd
from t
window w as (partition by class order by score nulls first);
```

Visualize the results that the view defines:

```plpgsql
select
  class, score, r, dr, to_char(pr, '90.99') as pr, to_char(cd, '90.99') as cd
from v
order by class, r;
```

This is the result:

```
 class | score  | r | dr |   pr   |   cd   
-------+--------+---+----+--------+--------
     1 | <null> | 1 |  1 |   0.00 |   0.17
     1 |      2 | 2 |  2 |   0.20 |   0.33
     1 |      4 | 3 |  3 |   0.40 |   0.50
     1 |      6 | 4 |  4 |   0.60 |   0.67
     1 |      8 | 5 |  5 |   0.80 |   0.83
     1 |     10 | 6 |  6 |   1.00 |   1.00
     2 |      2 | 1 |  1 |   0.00 |   0.20
     2 |      4 | 2 |  2 |   0.25 |   0.40
     2 |      6 | 3 |  3 |   0.50 |   0.60
     2 |      8 | 4 |  4 |   0.75 |   0.80
     2 |     10 | 5 |  5 |   1.00 |   1.00
```

Now, simulate the hypothetical insert of two new rows, one in each class, and visualize the values that the four functions of interest produce for them. Do this within a transaction that you rollback.

```plpgsql
start transaction;
insert into t(k, class, score) values (21, 1, 5), (22, 2, 6);

select
  class, score, r, dr, to_char(pr, '90.99') as pr, to_char(cd, '90.99') as cd
from v
where k in (21, 22)
order by class, r;

rollback;
```

This is the result:

```
 class | score | r | dr |   pr   |   cd   
-------+-------+---+----+--------+--------
     1 |     5 | 4 |  4 |   0.50 |   0.57
     2 |     6 | 3 |  3 |   0.40 |   0.67
```

Next, create a table function, parameterized by the value of the hypothetical to-be-inserted score, to show the result of the "Within-group hypothetical-set" invocation of the four functions of interest:

```plpgsql
drop function if exists h(int) cascade;
create function h(hypothetical_score in int)
  returns table(
    class int,
    score int,
    r bigint,
    dr bigint,
    pr double precision,
    cd double precision)
  language sql
as $body$
  with v as (
    select
      class,
      rank(hypothetical_score)         within group (order by score nulls first) as r,
      dense_rank(hypothetical_score)   within group (order by score nulls first) as dr,
      percent_rank(hypothetical_score) within group (order by score nulls first) as pr,
      cume_dist(hypothetical_score)    within group (order by score nulls first) as cd
    from t
    group by class)
  select class, hypothetical_score as score, r, dr, pr, cd from v;
$body$;
```

First invoke it with a hypothetical score of _5_:

```plpgsql
select
  class, score, r, dr, to_char(pr, '90.99') as pr, to_char(cd, '90.99') as cd
from h(5)
order by class;
```

This is the result:

```
 class | score | r | dr |   pr   |   cd   
-------+-------+---+----+--------+--------
     1 |     5 | 4 |  4 |   0.50 |   0.57
     2 |     5 | 3 |  3 |   0.40 |   0.50
```

Now invoke it with a hypothetical score of _6_:

```plpgsql
select
  class, score, r, dr, to_char(pr, '90.99') as pr, to_char(cd, '90.99') as cd
from h(6)
order by class;
```

This is the result:

```
 class | score | r | dr |   pr   |   cd   
-------+-------+---+----+--------+--------
     1 |     6 | 4 |  4 |   0.50 |   0.71
     2 |     6 | 3 |  3 |   0.40 |   0.67
```

Notice that, as promised, each invocation produces _two_ rows—one row for each of the two distinct values of _"class"_.

It's easy to produce the identical result that simulating the hypothetical inserts within a rolled back transaction produced:

```plpgsql
with v as (
  select class, score, r, dr, pr, cd from h(5) where class = 1
  union all
  select class, score, r, dr, pr, cd from h(6) where class = 2)
select
  class, score, r, dr, to_char(pr, '90.99') as pr, to_char(cd, '90.99') as cd
from v
order by class;
```

This is the result:

```
 class | score | r | dr |   pr   |   cd   
-------+-------+---+----+--------+--------
     1 |     5 | 4 |  4 |   0.50 |   0.57
     2 |     6 | 3 |  3 |   0.40 |   0.67
```

This is indeed identical to the result that the simulated two hypothetical inserts produced.