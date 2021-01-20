---
title: first_value(), nth_value(), last_value()
linkTitle: first_value(), nth_value(), last_value()
headerTitle: first_value(), nth_value(), last_value()
description: Describes the functionality of the YSQL window functions first_value(), nth_value(), and last_value().
menu:
  latest:
    identifier: first-value-nth-value-last-value
    parent: window-function-syntax-semantics
    weight: 30
isTocNested: true
showAsideToc: true
---

These three window functions fall into the second group, [Window functions that return column(s) of another row within the window](../#window-functions-that-return-column-s-of-another-row-within-the-window) in the section [List of all window functions](../#list-of-all-window-functions). Each of the functions in the second group makes obvious sense when the scope within which the specified row is found is the entire [_window_](../../invocation-syntax-semantics/#the-window-definition-rule). Only this use will be described here. When used this way, each of these functions, as their names suggest, return the same result for each row in the current [_window_](../../invocation-syntax-semantics/#the-window-definition-rule). See  [Examples that illustrate all three functions](#examples-that-illustrate-all-three-functions) below.

If you have a use case that requires a specifically tailored [_window frame_](../../invocation-syntax-semantics/#frame-clause-semantics-for-window-functions), then see the section [The frame_clause](../../invocation-syntax-semantics/#the-frame-clause).

## first_value()

**Signature:**

```
input value:       anyelement
return value:      anyelement
```

**Purpose:** Return the specified value from the first row, in the specified sort order, in the current [_window frame_](../../invocation-syntax-semantics/#frame-clause-semantics-for-window-functions). If you specify the [`frame_clause`](../../../../syntax_resources/grammar_diagrams/#frame-clause) to start at a fixed offset before the current row, then `first_value()` would produce the same result as would the correspondingly parameterized `lag()`. If this is your aim, then you should use `lag()` for clarity.

## nth_value()

**Signature:**

```
input value:       anyelement, int
return value:      anyelement
```

**Purpose:** Return the specified value from the "_Nth"_ row, in the specified sort order, in the current [_window frame_](../../invocation-syntax-semantics/#frame-clause-semantics-for-window-functions). The second, mandatory, parameter specifies _"N"_ in _"Nth"_.

## last_value()

**Signature:**

```
input value:       anyelement
return value:      anyelement
```

**Purpose:** Return the specified value from the last row, in the specified sort order, in the current [_window frame_](../../invocation-syntax-semantics/#frame-clause-semantics-for-window-functions).

## Examples that illustrate all three functions

{{< note title=" " >}}

If you haven't yet installed the tables that the code examples use, then go to the section [The data sets used by the code examples](../data-sets/).

{{< /note >}}

This example uses table _"t1"_. Notice that it has been contrived so that the last _"v"_ (ordered by _"k"_) for each value of _"class"_ is `NULL`.

Use the technique shown in the section [Using `nth_value()` and `last_value()` to return the whole row](../#using-nth-value-and-last-value-to-return-the-whole-row) so that each of the three window functions produces all of the fields in each row:

```plpgsql
drop type if exists rt cascade;
create type rt as (class int, k int, v int);

select
  class,
  k,
  first_value((class, k, v)::rt::text)    over w as fv,
  nth_value  ((class, k, v)::rt::text, 3) over w as nv,
  last_value ((class, k, v)::rt::text)    over w as lv
from t1
window w as (
  partition by class
  order by k
  range between unbounded preceding and unbounded following);
```

Here is the result. To make it easier to see the pattern, a break has been manually inserted here between each successive set of rows with the same value for _"class"_.

```
 class | k  |    fv     |    nv     |   lv    
-------+----+-----------+-----------+---------
     1 |  1 | (1,1,1)   | (1,3,3)   | (1,5,)
     1 |  2 | (1,1,1)   | (1,3,3)   | (1,5,)
     1 |  3 | (1,1,1)   | (1,3,3)   | (1,5,)
     1 |  4 | (1,1,1)   | (1,3,3)   | (1,5,)
     1 |  5 | (1,1,1)   | (1,3,3)   | (1,5,)

     2 |  6 | (2,6,6)   | (2,8,8)   | (2,10,)
     2 |  7 | (2,6,6)   | (2,8,8)   | (2,10,)
     2 |  8 | (2,6,6)   | (2,8,8)   | (2,10,)
     2 |  9 | (2,6,6)   | (2,8,8)   | (2,10,)
     2 | 10 | (2,6,6)   | (2,8,8)   | (2,10,)

     3 | 11 | (3,11,11) | (3,13,13) | (3,15,)
     3 | 12 | (3,11,11) | (3,13,13) | (3,15,)
     3 | 13 | (3,11,11) | (3,13,13) | (3,15,)
     3 | 14 | (3,11,11) | (3,13,13) | (3,15,)
     3 | 15 | (3,11,11) | (3,13,13) | (3,15,)

     4 | 16 | (4,16,16) | (4,18,18) | (4,20,)
     4 | 17 | (4,16,16) | (4,18,18) | (4,20,)
     4 | 18 | (4,16,16) | (4,18,18) | (4,20,)
     4 | 19 | (4,16,16) | (4,18,18) | (4,20,)
     4 | 20 | (4,16,16) | (4,18,18) | (4,20,)

     5 | 21 | (5,21,21) | (5,23,23) | (5,25,)
     5 | 22 | (5,21,21) | (5,23,23) | (5,25,)
     5 | 23 | (5,21,21) | (5,23,23) | (5,25,)
     5 | 24 | (5,21,21) | (5,23,23) | (5,25,)
     5 | 25 | (5,21,21) | (5,23,23) | (5,25,)
```

Notice that the `::text` typecast of a _"row"_ type value renders `NULL` simply as an absence. This explains why you see, for example, _"(1,5,)"_ for each value produced by `last_value()` in the [_window_](../../invocation-syntax-semantics/#the-window-definition-rule) where _"k=1"_. This basic example certainly demonstrates the meaning of _"first"_, _"Nth"_ (for _"N=3"_), and _"last"_. But it isn't very useful because, just as these names suggest, the output is the same for each row in a particular [_window_](../../invocation-syntax-semantics/#the-window-definition-rule). The following query adds a conventional `GROUP BY` clause. It also extracts the interesting fields from the _"row"_ type value that each window function produces as individual values.

```plpgsql
drop type if exists rt cascade;
create type rt as (class int, k int, v int);
\pset null '??'

with a as (
  select
    class,
    first_value((class, k, v)::rt)    over w as fv,
    nth_value  ((class, k, v)::rt, 3) over w as nv,
    last_value ((class, k, v)::rt)    over w as lv
  from t1
  window w as (
    partition by class
    order by k
    range between unbounded preceding and unbounded following))
select
  (fv).class as fv_class,
  (fv).k     as fv_k,
  (fv).v     as fv_v,
  (nv).k     as nv_k,
  (nv).v     as nv_v,
  (lv).k     as lv_k,
  (lv).v     as lv_v
from a
group by
  (fv).class,
  (fv).k,
  (fv).v,
  (nv).k,
  (nv).v,
  (lv).k,
  (lv).v
order by 1;
```

This is the result:

```
 fv_class | fv_k | fv_v | nv_k | nv_v | lv_k | lv_v 
----------+------+------+------+------+------+------
        1 |    1 |    1 |    3 |    3 |    5 |   ??
        2 |    6 |    6 |    8 |    8 |   10 |   ??
        3 |   11 |   11 |   13 |   13 |   15 |   ??
        4 |   16 |   16 |   18 |   18 |   20 |   ??
        5 |   21 |   21 |   23 |   23 |   25 |   ??
```
