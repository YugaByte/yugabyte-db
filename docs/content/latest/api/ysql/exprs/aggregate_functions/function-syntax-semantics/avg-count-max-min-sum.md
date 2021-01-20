---
title: avg(), count(), max(), min(), sum()
linkTitle: avg(), count(), max(), min(), sum()
headerTitle: avg(), count(), max(), min(), sum()
description: Describes the functionality of the avg(), count(), max(), min(), sum() YSQL aggregate functions
menu:
  latest:
    identifier: avg-count-max-min-sum
    parent: aggregate-function-syntax-semantics
    weight: 10
isTocNested: true
showAsideToc: true
---

Each of the aggregate functions [`avg()`](./#avg), [`count()`](./#count), [`max()`](./#max-min), [`min()`](./#max-min), and [`sum()`](./#sum) is invoked by using the same syntax—either the [`GROUP BY` syntax](./#group-by-syntax) or the [`OVER` syntax](./#over-syntax).

## avg()

**Signature:**

```
input value:       smallint, int, bigint, numeric, real, double precision, interval
                   
return value:      numeric, double precision, interval
```

**Notes:** The lists of input and return data types give the distinct kinds. Briefly, the input can be any of the data types for which the notion of average is meaningful—including, therefore, the data types whose values are constrained to be whole numbers. Because, for example, the average of the whole numbers _3_ and _4_ is _3.5_, the return data type is never one whose values are constrained to be whole numbers. Here are the specific mappings:

```
INPUT             OUTPUT
----------------  ----------------
smallint          numeric
int               numeric
bigint            numeric
numeric           numeric
real              double precision
double precision  double precision
interval          interval
```

**Purpose:** Computes the arithmetic mean of a set of summable values by adding them all together and dividing by the number of values. If the set contains `null`s, then these are simply ignored—both when computing the sum and when counting the number of values.

## count()

**Signature:**

```
input value:       "any"
                   
return value:      bigint
```

**Purpose:** Counts the number of `non null` values in a set. The data type of the values is of no consequence.

## max(), min()

**Signature:** `max()` and `min()` have the same signature as each other.

```
input value:       smallint, int, bigint, numeric, real, double precision, money,
                   character, varchar, text,
                   date, abstime, time with time zone, time without time zone,
                   timestamp with time zone, timestamp without time zone, tid,
                   interval, oid, inet, anyenum, anyarray
                   
return value:      < Same as the input value's data type except when the input is "varchar".
                     When the input is "varchar", the return is "text" >
```

**Purpose:** Computes the greatest, or the least, value among the values in the set using the rule that is used for the particular data type in the `ORDER BY` clause. `null`s are removed before sorting the values.

## sum()

**Signature:**

```
input value:       smallint, int, bigint, numeric, real, double precision, money, interval

return value:      bigint, numeric, double precision, real, money, interval
```

**Notes:** The lists of input and return data types give the distinct kinds. Briefly, the input can be any of the data types for which the notion of summation is meaningful. Here are the specific mappings:

```
INPUT             OUTPUT
----------------  ----------------
smallint          bigint
int               bigint
bigint            numeric
numeric           numeric
real              real
double precision  double precision
money             money
interval          interval
```

**Purpose:** Computes the sum of a set of summable values by adding them all together.  If the set contains `null`s, then these are simply ignored.

## Examples

First create and populate the test table:

```plpgsql
drop table if exists t cascade;
create table t(
  k     int      primary key,
  class int      not null,
  n     numeric,
  t     text     not null);

insert into t(k, class, n, t)
select
  (1 + s.v),
  case (s.v) < 5
    when true then 1
              else 2
  end,
  case (s.v) <> 4
    when true then (100.0 + s.v)::numeric
              else null
  end,
  chr(97 + s.v)
from generate_series(0, 9) as s(v);

\pset null <null>
select k, class, n, t from t order by k;
```

This is the result:

```
 k  | class |   n    | t 
----+-------+--------+---
  1 |     1 |    100 | a
  2 |     1 |    101 | b
  3 |     1 |    102 | c
  4 |     1 |    103 | d
  5 |     1 | <null> | e
  6 |     2 |    105 | f
  7 |     2 |    106 | g
  8 |     2 |    107 | h
  9 |     2 |    108 | i
 10 |     2 |    109 | j
```
### GROUP BY syntax

Now try this query:

```plpgsql
select
  class,

  -- "numeric" arguments.
  to_char(avg(n), '990.99')      as "avg(n)",
  count(n)                       as "count(n)",
  min(n)                         as "min(n)",
  max(n)                         as "max(n)",
  sum(n)                         as "sum(n)",

  -- "text" arguments.
  count((chr(n::int)||t)::text)  as "count(expr)",
  min(t)                         as "min(t)",
  max(t)                         as "max(t)"
from t
group by class
order by class;
```

It produces this result:

```
 class | avg(n)  | count(n) | min(n) | max(n) | sum(n) | count(expr) | min(t) | max(t) 
-------+---------+----------+--------+--------+--------+-------------+--------+--------
     1 |  101.50 |        4 |    100 |    103 |    406 |           4 | a      | e
     2 |  107.00 |        5 |    105 |    109 |    535 |           5 | f      | j
```

The results for _"class = 1"_ show a generic property of aggregate functions: if, for some row, the expression that is used to invoke the function evaluates to `null`, then that row is silently ignored and the result is calculated by using only the `not null` values in the designated [window](../../../window_functions/invocation-syntax-semantics/#the-window-definition-rule).

Here's a more dramatic illustration of the same rule:

```plpgsql
drop table if exists t cascade;
create table t2(
  k     int      primary key,
  n     numeric);

insert into t2(k, n)
select
  s.v,
  case
    when (s.v) between 1 and 3 then 1
    when (s.v) between 4 and 6 then 2
                               else null
  end
from generate_series(1, 9) as s(v);

\pset null <null>
select k, n from t2 order by k;
```

This is the result:

```plpgsql
 k |   n    
---+--------
 1 |      1
 2 |      1
 3 |      1
 4 |      2
 5 |      2
 6 |      2
 7 | <null>
 8 | <null>
 9 | <null>
```

Now try this:

```plpgsql
select count(distinct n) from t2;
```

Here, the `count()` expression uses the `distinct` keyword. This is the result:

```
 count 
-------
     2
```

Because `null` means, quite literally, the complete absence of information, it is excluded from the set of to-be-processed values.

### OVER syntax

Here is a "mechanical" re-write of the [`GROUP BY` syntax](./#group-by-syntax) code example above.

```plpgsql
select
  class,

  -- "numeric" arguments.
  n,
  to_char(avg(n) filter (where n not in (102, 107)) over w, '990.99')  as "avg(n)",
  count(n)       filter (where n not in (102, 107)) over w             as "count(n)",
  min(n)         filter (where n not in (102, 107)) over w             as "min(n)",
  max(n)         filter (where n not in (102, 107)) over w             as "max(n)",
  sum(n)         filter (where n not in (102, 107)) over w             as "sum(n)",

  -- "text" arguments.
  t,
  count((chr(n::int)||t)::text) over w                                 as "count(expr)",
  min(t) over w                                                        as "min(t)",
  max(t) over w                                                        as "max(t)"
from t
window w as (partition by class order by n range between unbounded preceding and current row)
order by class;
```

The `GROUP BY` clause has been removed; and the [`WINDOW` definition clause](../../../window_functions/invocation-syntax-semantics/#definition-of-the-fn-invocation-rule-and-the-window-definition-rule) has been added. And each aggregate function is invoked using the syntax defined by the [fn_over_window](../../../../syntax_resources/grammar_diagrams/#fn-over-window) rule. This is explained in the [Semantics](../../../window_functions/invocation-syntax-semantics/#semantics) section within the [Window function invocation—SQL syntax and semantics](../../../window_functions/invocation-syntax-semantics/) section. Notice that, in this example, the `FILTER` clause is used only for the aggregate function invocations that use `numeric` arguments.

Here is the result. Whitespace has been added manually around each of the rows that the `FILTER` clause identifies.

```
 class |   n    | avg(n)  | count(n) | min(n) | max(n) | sum(n) | t | count(expr) | min(t) | max(t) 
-------+--------+---------+----------+--------+--------+--------+---+-------------+--------+--------
     1 |    100 |  100.00 |        1 |    100 |    100 |    100 | a |           1 | a      | a
     1 |    101 |  100.50 |        2 |    100 |    101 |    201 | b |           2 | a      | b

     1 |    102 |  100.50 |        2 |    100 |    101 |    201 | c |           3 | a      | c

     1 |    103 |  101.33 |        3 |    100 |    103 |    304 | d |           4 | a      | d
     1 | <null> |  101.33 |        3 |    100 |    103 |    304 | e |           4 | a      | e
     2 |    105 |  105.00 |        1 |    105 |    105 |    105 | f |           1 | f      | f
     2 |    106 |  105.50 |        2 |    105 |    106 |    211 | g |           2 | f      | g

     2 |    107 |  105.50 |        2 |    105 |    106 |    211 | h |           3 | f      | h

     2 |    108 |  106.33 |        3 |    105 |    108 |    319 | i |           4 | f      | i
     2 |    109 |  107.00 |        4 |    105 |    109 |    428 | j |           5 | f      | j
```



The choice, `range between unbounded preceding and current row`, for the [`frame_clause`](../../../../syntax_resources/grammar_diagrams/#frame-clause), was made because its effect is easy to understand and this lets you easily predict the values that the aggregate functions should produce for each successive row within the specified [window](../../../window_functions/invocation-syntax-semantics/#the-window-definition-rule).

Notice that the `FILTER` clause has no effect on the number of rows that the subquery defines. Rather, it determines only what rows within the [window](../../../window_functions/invocation-syntax-semantics/#the-window-definition-rule) are considered by the particular aggregate function whose invocation is governed by that clause. The values shown in the columns _"avg(n)"_, _"count(n)"_, _"min(n)"_, _"max(n)"_, and _"sum(n)"_ are the same for the filtered-out row with _"n = 102"_ as they are for the previous row in the sorting order with _"n = 101"_. The same holds for the filtered-out row with _"n = 107"_ and the previous row with _"n = 106"_.

The section [Using the aggregate function avg() to compute a moving average](../../../window_functions/functionality-overview/#using-the-aggregate-function-avg-to-compute-a-moving-average) shows how `avg()`, invoked with the `OVER` syntax, can be used to this effect.
