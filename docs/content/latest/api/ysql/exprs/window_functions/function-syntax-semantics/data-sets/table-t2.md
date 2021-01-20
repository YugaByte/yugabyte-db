---
title: table t2
linkTitle: table t2
headerTitle: Create and populate table t2
description: Creates and populate table t2 with data that allows the demonstration of the YSQL window functions.
menu:
  latest:
    identifier: table-t2
    parent: data-sets
    weight: 30
isTocNested: true
showAsideToc: true
---

{{< note title=" " >}}

Make sure that you read the section [The data sets used by the code examples](../../data-sets/) before running the script to create table _"t2"_. In particular, it's essential that you have installed the [pgcrypto](../../../../../extensions/#pgcrypto) extension.

{{< /note >}}

The rows in table  _"t2"_ are inserted in random order. It is used for demonstrating these window functions:
[`row_number()`](../../row-number-rank-dense-rank/#row-number),
[`rank()`](../../row-number-rank-dense-rank/#rank),
[`dense_rank()`](../../row-number-rank-dense-rank/#dense-rank),
[`percent_rank()`](../../percent-rank-cume-dist-ntile/#percent-rank),
[`cume_dist()`](../../percent-rank-cume-dist-ntile/#cume-dist),
and [`ntile()`](../../percent-rank-cume-dist-ntile/#ntile).

It contains eighteen rows. It has primary key _"k"_ (`int`) whose values are the dense sequence from _1_ through _18_. The column _"class"_ (`int`) has two distinct values, _1_ and _2_, and there are nine rows with each _"class"_ value.
The values for the column _"score"_, for _"class = 1"_, are the dense sequence from _1_ through _9_.
And for _"class = 2"_, there are deliberate duplicates so the _2_ and _8_ each occur twice and _1_ and _9_ are missing. This scheme allows the difference between `rank()` and `dense_rank()` to be seen.

For maximum pedagogic effect, it uses the same technique that [table t1](../table-t1/) uses to ensure that, with no window `ORDER BY` clause, a `SELECT` will return rows in a random order.

This `ysqlsh` script creates and populates able _"t2"_. Save it as `t2.sql`.

```plpgsql
-- Suppress the spurious warning that is raised
-- when the to-be-deleted table doesn't yet exist.
set client_min_messages = warning;
drop type if exists rt cascade;
drop table if exists t2;

create table t2(
  surrogate_pk uuid primary key,
  class int not null,
  k int not null,
  score int not null,
  constraint t2_pk unique(class, k),
  constraint t2_k_positive check(k > 0),
  constraint t2_class_positive check(class > 0));

insert into t2(surrogate_pk, class, k, score)
with
  v1 as (
    values
      (1,  1, 1),
      (1,  2, 2),
      (1,  3, 3),
      (1,  4, 4),
      (1,  5, 5),
      (1,  6, 6),
      (1,  7, 7),
      (1,  8, 8),
      (1,  9, 9),

      (2, 10, 2),
      (2, 11, 2),
      (2, 12, 2),
      (2, 13, 4),
      (2, 14, 5),
      (2, 15, 6),
      (2, 16, 7),
      (2, 17, 7),
      (2, 18, 9)),
  v2 as (
    select
      gen_random_uuid() as r,
      column1,
      column2,
      column3
    from v1)
select
  r,
  column1,
  column2,
  column3
from v2
order by r;
```
Now inspect its contents:

```plpgsql
-- Notice the absence of "ORDER BY".
select class, k, score
from t2;

select class, k, score
from t2
order by class, k;
```
Here is the result of the second `SELECT`. To make it easier to see the pattern, several blank lines have been manually inserted here between each successive set of rows with the same value for _"class"_. And in the second set, which has ties, one blank line has been inserted between each tie group.
```
 class | k  | score 
-------+----+-------
     1 |  1 |     1
     1 |  2 |     2
     1 |  3 |     3
     1 |  4 |     4
     1 |  5 |     5
     1 |  6 |     6
     1 |  7 |     7
     1 |  8 |     8
     1 |  9 |     9



     2 | 10 |     2
     2 | 11 |     2
     2 | 12 |     2

     2 | 13 |     4

     2 | 14 |     5

     2 | 15 |     6

     2 | 16 |     7
     2 | 17 |     7

     2 | 18 |     9
```

