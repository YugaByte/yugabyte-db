---
title: mode(), percentile_disc(), percentile_cont()
linkTitle: mode(), percentile_disc(), percentile_cont()
headerTitle: Within-group ordered-set aggregate functions
description: Describes the Within-group ordered-set functionality of the mode(), percentile_disc(), percentile_cont() YSQL aggregate functions
menu:
  latest:
    identifier: mode-percentile-disc-percentile-cont
    parent: aggregate-function-syntax-semantics
    weight: 90
isTocNested: true
showAsideToc: true
---

This section describes the use of [`mode()`](./#mode), [`percentile_disc()`](./#percentile-disc-percentile-cont), and [`percentile_cont()`](./#percentile-disc-percentile-cont) as "within-group ordered-set" aggregate functions.

The functionality of the `mode()` aggregate function differs from that of the mutually closely related `percentile_disc()`, `percentile_cont()` aggregate function pair. All three are described in the same section because they share the same invocation syntax:

```
select
  grouping_column_1, grouping_column_1,...
  the_function(function-specific arg list) within group (order by ordering expression)
from t
group by grouping_column_1, grouping_column_1,...
order by grouping_column_1, grouping_column_1,...
```

The syntax gives this kind of aggregate function its name.

You might wonder why this particular syntax is required rather than the usual syntax that, for example, the `max()` and `min()` aggregate functions require. There is no underlying semantic reason. Rather, YugabyteDB inherits this from PostgreSQL. Other RDBMSs do use the same syntax for the  _"mode()"_ functionality as YugabyteDB uses for `max()` and `min()`—but they might spell the name of the function differently. Don't worry about this—the within-group ordered-set is sufficient to implement the required functionality.

## mode()

**Signature:**

```
input value:       <none>
                   togther with "WITHIN GROUP" and "ORDER BY anyelement" invocation syntax 
return value:      anyelement
```

**Purpose:** Return the most frequent value of _"sort expression"_. If there's more than one equally-frequent value, then one of these is silently chosen arbitrarily.

### Simple example without GROUP BY

In this first contrived example, the  "_ordering expression"_ uses the `lower()` function and refers to two columns. It also evaluates to `null` for one row.

```plpgsql
drop table if exists t cascade;
create table t(
  k     int   primary key,
  v1    text,
  v2    text);

insert into t(k, v1, v2) values
  (1, 'dog',   'food'),
  (2, 'cat',   'flap'),
  (3, null,    null),
  (4, 'zebra', 'stripe'),
  (5, 'frog',  'man'),
  (6, 'fish',  'soup'),
  (7, 'ZEB',   'RASTRIPE');
```

First, use `count(*)` ordinarily to determine how often each value produced by the chosen expression occurs.

```plpgsql
\pset null <null>

select
  lower(v1||v2) as "ordering expr",
  count(*) as n
from t
group by lower(v1||v2)
order by n desc, lower(v1||v2) asc nulls last;
```

This is the result:

```
 ordering expr | n 
---------------+---
 zebrastripe   | 2
 catflap       | 1
 dogfood       | 1
 fishsoup      | 1
 frogman       | 1
 <null>        | 1
```

Some RDBMSs don't implement a _"mode()"_ function but require you to use a query like this together with further syntax to pick out the top hit. Here is the YugabyteDB approach:

```plpgsql
select
  mode() within group (order by lower(v1||v2)) as "most frequent ordering expr"
from t;
```

This is the result:

```
 ordering expr 
---------------
 zebrastripe
```

### Example that uses GROUP BY

In this second example, the table has a grouping column to show how the `WITHIN GROUP ORDER BY` syntax is used together with the `GROUP BY` clause. The data is contrived so that the value _"v = 37"_ occurs twice for _"class = 1"_ and so that the value _"v = 42"_ occurs twice for _"class = 2"_. Otherwise each distinct value of _"v"_ occurs just once.

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
The [`dense_rank()`](../../../window_functions/function-syntax-semantics/row-number-rank-dense-rank/#dense-rank) window function provides an effective way to inspect the result:

```plpgsql
with
  a1 as (
  select class, v, count(*) as n
  from t
  group by class, v)

select
  class,
  n,
  v,
  dense_rank() over (partition by class order by n desc) as r
from a1
order by class, r;
````

This is the result:

```
 class | n | v  | r 
-------+---+----+---
     1 | 2 | 37 | 1
     1 | 1 |  2 | 2
     1 | 1 | 10 | 2
     1 | 1 |  1 | 2
     1 | 1 |  7 | 2
     1 | 1 |  9 | 2
     1 | 1 |  8 | 2
     1 | 1 |  4 | 2
     1 | 1 |  3 | 2
     
     2 | 3 | 42 | 1
     2 | 1 | 20 | 2
     2 | 1 | 13 | 2
     2 | 1 | 12 | 2
     2 | 1 | 18 | 2
     2 | 1 | 14 | 2
     2 | 1 | 19 | 2
     2 | 1 | 11 | 2
```

The blank line was added manually to improve readability.

It's easy, now, to elaborate the query with a second `WITH` clause view to pick out the row from each class whose dense rank is one—in other words the modal value. (If the data had more than one row with a dense rank of one, then they would all be picked out.)

```plpgsql
with
  a1 as (
    select class, v, count(*) as n
    from t
    group by class, v),

  a2 as (
    select
      class,
      n,
      v,
      dense_rank() over (partition by class order by n desc) as r
    from a1)

select class, n, v
from a2
where r = 1
order by class;
```

This is the result:

```
 class | n | v  
-------+---+----
     1 | 2 | 37
     2 | 3 | 42
```

Here's how to use the `mode()` within-group ordered-set aggregate function to get the modal value in each class.

```plpgsql
select
  class,
  mode() within group (order by v)
from t
group by class
order by class;
```

This is the result:

```
 class | mode 
-------+------
     1 |   37
     2 |   42
```

The query that gets just the modal value for each class using the `mode()` function is very much simpler that the one that gets _both_ the modal value _and_ its frequency of occurrence using the `dense_rank()` window function. It would be possible to involve the basic use of `mode()` in a `WITH` clause construct that defines other subqueries to get both the modal value and its frequency of occurrence that way. But this would bring the complexity up to a similar level to that of the query that uses `dense_rank()` to this end.

## percentile_disc(), percentile_cont()

These two aggregate functions are closely related. Briefly, they implement the inverse of the functionality that the [`percent_rank()`](../../../window_functions/function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank) window function implements. For this reason, they are sometimes referred to as “inverse distribution” functions.

**Signature:**

```
-- First overload.
input value:       double precision
                   togther with "WITHIN GROUP" and "ORDER BY anyelement" invocation syntax 
return value:      anyelement

-- Second overload.
input value:       double precision[]
                   togther with "WITHIN GROUP" and "ORDER BY anyelement" invocation syntax 
return value:      anyarray
```

**Purpose:** Each scalar overload of `percentile_disc()` and `percentile_cont()` takes a percentile rank value as input and returns the value, within the specified [window](../../../window_functions/invocation-syntax-semantics/#the-window-definition-rule), that would produce this. The window is specified by `WITHIN GROUP` in conjunction with the `ORDER BY` clause, and by the optional `GROUP BY` clause, in the subquery that invokes the aggregate function in its `SELECT` list.

- `percentile_disc()` (the discrete variant) simply returns the first actual value (in the ordered set of candidate values in the window) that is equal to, or greater than, the value that would produce the specified percentile rank value.

- `percentile_cont()` (the continuous variant) does just the same as `percentile_disc()` if there happens to be an exact match; otherwise it finds the immediately smaller value and the immediately bigger value and interpolates between them.

Each array overload takes an array of percentile rank values as input and returns the array of values that would produce these according to the rules for the discrete and continuous variants specified above.

### Basic example using the scalar overloads

First, create a table with a set of ten successive prime numbers starting from an arbitrary prime.

```plpgsql
drop table if exists t cascade;
create table t(v double precision primary key);

insert into t(v)
values (47), (53), (59), (61), (67), (71), (73), (83), (89), (97);
```
Now, note what [`percent_rank()`](../../../window_functions/function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank) produces:

```plpgsql
with a as (
  select
    v,
    percent_rank() over (order by v) as pr
  from t) 
select
  to_char(v,  '90.99')              as v,
  to_char(pr, '90.999999999999999') as pr
from a;
```

This is the result:

```
   v    |         pr         
--------+--------------------
  47.00 |   0.00000000000000
  53.00 |   0.11111111111111
  59.00 |   0.22222222222222
  61.00 |   0.33333333333333
  67.00 |   0.44444444444444
  71.00 |   0.55555555555556
  73.00 |   0.66666666666667
  83.00 |   0.77777777777778
  89.00 |   0.88888888888889
  97.00 |   1.00000000000000
```

Next, choose the value `0.66666666666667` from the _"pr"_ column. Notice that this corresponds to the value `73.00` from the _"v"_ column. Use the chosen _"pr"_ value as input to the scalar overloads of `percentile_disc()` and `percentile_cont()`.

```plpgsql
with a as (
  select
    percentile_disc(0.66666666666667) within group (order by v) as p_disc,
    percentile_cont(0.66666666666667) within group (order by v) as p_cont
  from t)
select
  to_char(a.p_disc, '90.99') as pd,
  to_char(a.p_cont, '90.99') as pc
from a;
```

This is the result:

```
   pd   |   pc   
--------+--------
  73.00 |  73.00
```

Notice that the value `73.00` from the _"v"_ column has been recovered—and that the discrete and continuous variants each produce the same result in this example.

Next, choose the value `0.61111111111112`. This does not occur in the _"pr"_ column but lies mid-way between `0.55555555555556` and `0.66666666666667`, produced respectively by `71.00` and `73.00` in the _"v"_ column.


```plpgsql
with a as (
  select
    percentile_disc(0.61111111111112) within group (order by v) as p_disc,
    percentile_cont(0.61111111111112) within group (order by v) as p_cont
  from t)
select
  to_char(a.p_disc, '90.99') as pd,
  to_char(a.p_cont, '90.99') as pc
from a;
```

This is the result:

```
   pd   |   pc   
--------+--------
  73.00 |  72.00
```

Notice that the _"pd"_ value, `73.00`, is the first _"v"_ value that is greater than `72.00`—and that the _"pc"_ value, `72.00`, has been interpolated between `71.00` and `73.00`.

### Basic example using the scalar overloads with GROUP BY

Re-create table _"t"_ with a _"class"_ column for grouping and display its contents:

```plpgsql
rop table if exists t2 cascade;
create table t2(
  class  int,
  v      double precision,
  constraint t_pk primary key(class, v));

insert into t2(class, v) values
  (1, 47), (1, 53), (1, 59), (1, 61), (1, 67),
  (2, 47), (2, 53), (2, 59), (2, 61), (2, 67), (2, 71);

with a as (
  select
    class,
    v,
    percent_rank() over (partition by class order by v) as pr
  from t2)
select
                          class,
  to_char(v,  '90.99') as v,
  to_char(pr, '90.99') as pr
from a
order by class, v;
```

This is the result:

```
   class |   v    |   pr   
-------+--------+--------
     1 |  47.00 |   0.00
     1 |  53.00 |   0.25
     1 |  59.00 |   0.50
     1 |  61.00 |   0.75
     1 |  67.00 |   1.00
     2 |  47.00 |   0.00
     2 |  53.00 |   0.20
     2 |  59.00 |   0.40
     2 |  61.00 |   0.60
     2 |  67.00 |   0.80
     2 |  71.00 |   1.00
```

Notice that there are five rows for _"class = 1"_ and six rows for _"class = 2"_.

Next, choose the value `0.50` from the _"pr"_ column. Notice that this corresponds to the value `59.00` from the _"v"_ column when _"class = 1"_ and it corresponds to a value  between`59.00` and `61.00` from the _"v"_ column when _"class = 2"_.

Use the chosen _"pr"_ value as input to the scalar overloads of `percentile_disc()` and `percentile_cont()`.

```plpgsql
with a as (
  select
    class,
    percentile_disc(0.50) within group (order by v) as p_disc,
    percentile_cont(0.50) within group (order by v) as p_cont
  from t2
  group by class)
select
  to_char(a.p_disc, '90.99') as pd,
  to_char(a.p_cont, '90.99') as pc
from a;
```

This is the result:

```
   pd   |   pc   
--------+--------
  59.00 |  59.00
  59.00 |  60.00
```

`percentile_disc()` returned the exact _"v"_ value when this was found and the immediately next larger value when it wasn't found. And `percentile_cont()` returned the exact _"v"_ value when this was found and it interpolated between the immediately enclosing smaller and larger values when it wasn't found.

### Determining the median value of a set of values

See [this Wikipedia entry](https://en.wikipedia.org/wiki/Median) on median.

The median of a set of values is defined, naïvely, as the value that divides the set into two same-sized subsets when the values are sorted in size order. Loosely stated, it's the "middle" value in the ordered set. The values on the smaller side are all less than, or equal to, the median value. And the values on the larger side are all greater than, or equal to, the median value. Edge cases (like, for example, when the set has an even number of unique values) are accommodated by a refinement of the naïve rule statement.

The advantage brought by using the median in describing data rather than using the mean (implemented by the `avg()` aggregate function) is that it is not skewed so much by a small proportion of extremely large or small values; it therefore probably gives a better idea of a "typical" value. For example, household income or asset ownership is usually characterized by the median value because just a few multi-billionaires can skew the mean.

Some SQL database systems have a built-in `median()` aggregate function. But PostgreSQL, and therefore YugabyteDB, do not. However, this is of no practical consequence because `percentile_cont(0.5)`, _by definition_, returns the median value.

Here are some examples and counter-examples.

```plpgsql
drop function  if exists  median_test()  cascade;
drop type      if exists  rt             cascade; 
drop table     if exists  t              cascade;

create table t(v double precision primary key);
insert into t(v)
select s.v from generate_series(11, 19) as s(v);

create type rt as (
  "count"             bigint,
  "avg"               text,
  "median"            text ,
  "count l.e. median" bigint,
  "count g.e. median" bigint);

create or replace function median_test_result()
  returns rt
  language sql
as $body$
with a as (
select
  avg(v)   as avg,
  percentile_cont(0.5) within group (order by v) as median
from t)
select
  (select count(*) from t),
  to_char(a.avg,    '90.99'),
  to_char(a.median, '90.99'),
  (select count(*) from t where v <= a.median),
  (select count(*) from t where v >= a.median)
from a;
$body$;

select * from median_test_result();
```

This is the result:

```
 count |  avg   | median | count l.e. median | count g.e. median 
-------+--------+--------+-------------------+-------------------
     9 |  15.00 |  15.00 |                 5 |                 5
```

Here, there is an _odd_ number of unique input values and the median is among these values. Now insert one extra unique value and repeat the test:

```plpgsql
insert into t(v) values(20);
select * from median_test_result();
```

This is the new result:

```
 count |  avg   | median | count l.e. median | count g.e. median 
-------+--------+--------+-------------------+-------------------
    10 |  15.50 |  15.50 |                 5 |                 5
```

Here, there is an _even_ number of unique input values and the median is _not_ among these values. Rather, it lies between the two existing values that are immediately less than, and immediately greater than, the median value.

In these first two examples, the median and the mean are the same. But here's an extreme edge case where, as promised, the median differs significantly from the mean because of the skewing effect of the single outlier value `1000.00`.:

```plpgsql
drop table if exists t cascade;
create table t(k serial primary key, v double precision not null);
insert into t(v)
select 1 from generate_series(1, 3)
union all
select 2 from generate_series(1, 10)
union all
select 3 from generate_series(1, 5)
union all
select 1000;

select * from median_test_result();

select to_char(v, '9990.00') as v from t order by v;
```

Here is the result:

```
 count |  avg   | median | count l.e. median | count g.e. median 
-------+--------+--------+-------------------+-------------------
    19 |  54.63 |   2.00 |                13 |                16
```

This shows that the naïve rule statement is insufficiently precise and that the design of `median_test_result()` function is too crude. Here are the contents of the table, where white space has been introduced manually:

```
    v     
----------
     1.00
     1.00
     1.00
     2.00
     2.00
     2.00
     2.00
     2.00
     2.00
     
     2.00
     
     2.00
     2.00
     2.00
     3.00
     3.00
     3.00
     3.00
     3.00
  1000.00
```

The number of rows (nine) on the lower side of the middle-most row with the median value, `2.00`, is indeed the same as the number of rows on the higher side of it. But the number of rows with a value less than or equal to the median value (thirteen) is different from the number of rows with a value greater than or equal to the median value (sixteen).

It isn't useful, here, to find unassailably precise wording to define the semantics of the median. The code illustrations are sufficient.

### Exhaustive example using the array overloads

Recreate table _"t"_ as it was defined and populated for the ["Basic example using the scalar overloads"](./#basic-example-using-the-scalar-overloads).

```plpgsql
drop table if exists t cascade;
create table t(v double precision primary key);

insert into t(v)
values (47), (53), (59), (61), (67), (71), (73), (83), (89), (97);
```

Now create a function that returns an array of all the values from the  _"pr"_ column for the demonstration of `percent_rank()` shown in that section.

```plpgsql
create or replace function pr_vals()
  returns double precision[]
  language plpgsql
as $body$
declare
  a constant double precision[] := (
    with a as (
      select percent_rank() over (order by v) as v from t) 
    select array_agg(v order by v) from a);
begin
  return a;
end;
$body$;
```

Now define three relations in a `WITH` clause that correspond to _"pr"_ values and the outputs of `percentile_disc()` and `percentile_cont()` from these inputs. Then inner-join these three relations. These comments are key:

- Think of the original table _"t(v double precision)"_ as an array _"v[]"_.
- Define the relation _"i, percent_rank(v[i])"_.
- Define the relation _"i, percentile_disc(percent_rank(v[i]))"_.
- Define the relation _"i, percentile_cont(percent_rank(v[i]))"_.
- Inner join them all using _"i"_.

```plpgsql
with
  -- Think of the original table "t(v double precision)" as an array "v[]".
  -- Define the relation "i, percent_rank(v[i])".
  pr_vals as (
    select
      g.i,
      (pr_vals())[g.i] as v
    from generate_subscripts(pr_vals(), 1) as g(i)),

  -- Define the relation "i, percentile_disc(percent_rank(v[i]))".
  pd_arr as (
    select
      percentile_disc(pr_vals()) within group (order by v) as v
    from t),
  pd_vals as (
    select
      g.i,
      (select v from pd_arr)[i] as v
    from generate_subscripts((select v from pd_arr), 1) as g(i)),

  -- Define the relation "i, percentile_cont(percent_rank(v[i]))".
  pc_arr as (
    select
      percentile_cont(pr_vals()) within group (order by v) as v
    from t),
  pc_vals as (
    select
      g.i,
      (select v from pc_arr)[i] as v
    from generate_subscripts((select v from pc_arr), 1) as g(i))

-- Inner join them all using "i".
select
  to_char(pr_vals.v, '90.999999999999999')  as pr,
  to_char(pd_vals.v, '90.99')               as "pd(pr)",
  to_char(pc_vals.v, '90.99')               as "pc(pr)"
from
  pr_vals
  inner join
  pd_vals using(i)
  inner join
  pc_vals using(i);
```

This is the result:

```
         pr         | pd(pr) | pc(pr) 
--------------------+--------+--------
   0.00000000000000 |  47.00 |  47.00
   0.11111111111111 |  53.00 |  53.00
   0.22222222222222 |  59.00 |  59.00
   0.33333333333333 |  61.00 |  61.00
   0.44444444444444 |  67.00 |  67.00
   0.55555555555556 |  71.00 |  71.00
   0.66666666666667 |  73.00 |  73.00
   0.77777777777778 |  83.00 |  83.00
   0.88888888888889 |  89.00 |  89.00
   1.00000000000000 |  97.00 |  97.00
```

Because the exact set of the ten _"pr"_ values from the  _"pr"_ column for the demonstration of `percent_rank()` shown above is used as input for both the discrete and continuous array overloads, each produces the same result: the exact set of _"v"_ values that produced the input set of _"pr"_ values.
