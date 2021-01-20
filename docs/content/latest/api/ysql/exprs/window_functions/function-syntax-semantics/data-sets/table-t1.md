---
title: table t1
linkTitle: table t1
headerTitle: Create and populate table t1
description: Creates and populate table t1 with data that allows the demonstration of the YSQL window functions.
menu:
  latest:
    identifier: table-t1
    parent: data-sets
    weight: 20
isTocNested: true
showAsideToc: true
---

{{< note title=" " >}}
Make sure that you read the section [The data sets used by the code examples](../../data-sets/) before running the script to create table _"t1"_. In particular, it's essential that you have installed the [pgcrypto](../../../../../extensions/#pgcrypto) extension.
{{< /note >}}

The rows in table  _"t1"_ are inserted in random order. The table is used for demonstrating these window functions:
[`percent_rank()`](../../percent-rank-cume-dist-ntile/#percent-rank),
[`cume_dist()`](../../percent-rank-cume-dist-ntile/#cume-dist),
and [`ntile()`](../../percent-rank-cume-dist-ntile/#ntile).
It is also used in the section [Informal overview of function invocation using the OVER clause](../../../functionality-overview/).


This `ysqlsh` script creates and populates able _"t1"_. Save it as `t1.sql`.

```plpgsql
-- Suppress the spurious warning that is raised
-- when the to-be-deleted table doesn't yet exist.
set client_min_messages = warning;
drop type if exists rt cascade;
drop table if exists t1;

create table t1(
  surrogate_pk uuid primary key,
  class int not null,
  k int not null,
  v int,
  constraint t1_class_k_unq unique(class, k),
  constraint t1_k_positive check(k > 0),
  constraint t1_class_positive check(class > 0));

insert into t1(surrogate_pk, class, k, v)
with
  a1 as (
    select generate_series(0, 24) as v1),
  a2 as (
    select
      (floor((v1*5)/25) + 1) as v2,
      v1
    from a1),
  a3 as (
    select
      gen_random_uuid() as r,
      v1,
      v2,
      (v2*5 + (v1 % 5) - v2*5 + 1) as v3
    from a2)
select
  r,
  v2,
  (v1 + 1),
  case
    when v3 between 0 and 4 then (v1 + 1)
    else                         null
  end
from a3
order by r;
```
Now inspect its contents. Notice that the first SELECT to display the content has no `ORDER BY`. This means that each time the script is rerun, the results that it produces will be ordered differently, as promised. If you repeatedly execute the first `SELECT` following a particular execution of the randomly ordered `INSERT`, then you'll probably see the same order time and again. The results of such an unordered `SELECT` tend to reflect the physical layout of the rows at the storage level; and this, in turn, tends to reflect the order of insertion—whatever that might have been, However, you _must not_ rely on this apparent rule. All sorts of things can intervene, especially over longer timescales, to change the ordering of the stored rows.

The second `SELECT` orders the rows usefully and, of course, produces a deterministically reliable result.

```plpgsql
\pset null '??'

-- Notice the absence of "ORDER BY".
select class, k, v
from t1;

select class, k, v
from t1
order by class, k;
```
Here is the result of the second `SELECT`. To make it easier to see the pattern, a blank line has been manually inserted here between each successive set of rows with the same value for _"class"_. (The same "break" technique is used to present the results that follow these.)
```
 class | k  | v  
-------+----+----
     1 |  1 |  1
     1 |  2 |  2
     1 |  3 |  3
     1 |  4 |  4
     1 |  5 | ??

     2 |  6 |  6
     2 |  7 |  7
     2 |  8 |  8
     2 |  9 |  9
     2 | 10 | ??

     3 | 11 | 11
     3 | 12 | 12
     3 | 13 | 13
     3 | 14 | 14
     3 | 15 | ??

     4 | 16 | 16
     4 | 17 | 17
     4 | 18 | 18
     4 | 19 | 19
     4 | 20 | ??

     5 | 21 | 21
     5 | 22 | 22
     5 | 23 | 23
     5 | 24 | 24
     5 | 25 | ??
```

Do the following to demonstrate that the result set produced by invoking a window function with an `OVER` clause whose [`window_definition`](../../../../../syntax_resources/grammar_diagrams/#window-definition) doesn't include a window `ORDER BY` clause is unreliable. Be sure to re-run the table creation and population script each time before you run this query:

```plpgsql
select
  class,
  k,
  row_number() over () as r
from t1
order by class, k;
```
Here is a typical result set:
```
 class | k  | r  
-------+----+----
     1 |  1 |  6
     1 |  2 | 18
     1 |  3 | 16
     1 |  4 | 14
     1 |  5 |  5
     
     2 |  6 | 20
     2 |  7 | 17
     2 |  8 |  8
     2 |  9 | 12
     2 | 10 | 23
     
     3 | 11 | 19
     3 | 12 | 25
     3 | 13 |  7
     3 | 14 | 24
     3 | 15 |  2
     
     4 | 16 |  1
     4 | 17 |  9
     4 | 18 | 11
     4 | 19 | 21
     4 | 20 | 22
     
     5 | 21 | 15
     5 | 22 |  4
     5 | 23 |  3
     5 | 24 | 10
     5 | 25 | 13
```
Finally, do this:

```postgresxql
select
  class,
  k,
  min(k) over (partition by class) as "min(k)"
from t1
order by class, k;
```
This is the result:
```
 class | k  | min(k) 
-------+----+--------
     1 |  1 |      1
     1 |  2 |      1
     1 |  3 |      1
     1 |  4 |      1
     1 |  5 |      1
     
     2 |  6 |      6
     2 |  7 |      6
     2 |  8 |      6
     2 |  9 |      6
     2 | 10 |      6
     
     3 | 11 |     11
     3 | 12 |     11
     3 | 13 |     11
     3 | 14 |     11
     3 | 15 |     11
     
     4 | 16 |     16
     4 | 17 |     16
     4 | 18 |     16
     4 | 19 |     16
     4 | 20 |     16
     
     5 | 21 |     21
     5 | 22 |     21
     5 | 23 |     21
     5 | 24 |     21
     5 | 25 |     21
```

This demonstrates a plausible use of the `OVER` clause whose [`window_definition`](../../../../../syntax_resources/grammar_diagrams/#window-definition) doesn't have a window `ORDER BY` clause but _does_ have a `PARTITION BY` clause. If you run and re-run the table creation and population script, then you'll see that the result set of the invocation of an aggregate function with an `OVER` clause is reliably reproducible.
