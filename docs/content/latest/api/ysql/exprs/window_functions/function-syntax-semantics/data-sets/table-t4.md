---
title: table t4
linkTitle: table t4
headerTitle: Create and populate table t4
description: Creates and populate table t4 with a psuedo-randomly generated approximate normal distrubution that allows the comparison of the YSQL percent_rank(), cume_dist(), and ntile() window functions.
menu:
  latest:
    identifier: table-t4
    parent: data-sets
    weight: 50
isTocNested: true
showAsideToc: true
---

{{< note title=" " >}}

Make sure that you read the section [The data sets used by the code examples](../../data-sets/) before running the script to create table _"t4"_. In particular, it's essential that you have installed the [pgcrypto](../../../../../extensions/#pgcrypto) and [tablefunc](../../../../../extensions/#tablefunc) extensions.

{{< /note >}}

The table _"t4"_ is used for comparing these window functions:
[`percent_rank()`](../../percent-rank-cume-dist-ntile/#percent-rank),
[`cume_dist()`](../../percent-rank-cume-dist-ntile/#cume-dist),
and [`ntile()`](../../percent-rank-cume-dist-ntile/#ntile).
See the section [Analyzing a normal distribution with percent_rank(), cume_dist(), and ntile()](../../../analyzing-a-normal-distribution/).

The table is populated using a procedure that is parameterized with the number of rows to generate. You will typically choose a large number like, 100,000. It uses the `normal_rand()` function to generate the specified number of  values by pseudorandomly picking values from an ideal normal distribution. The  `normal_rand()` function is brought by the [tablefunc](../../../../../extensions/#tablefunc) extension. This function is parameterized by the number of values to create, and by the mean and the standard deviation of the distribution from which to pick the values. An infinite number of such values would range between minus and plus infinity. But of course, some number, like _100,000_, of such values will lie between finite limits. It's sufficient for the purposes of the demonstrations that will use this data to scale the values so that the minimum is _0.0_ and the maximum is _100.0_. Doing this has the consequence that the mean will be about _50.0_ and the standard deviation will be about _10.0_—no matter what values for these are provided as the actual arguments to `normal_rand()`. It's sufficient to say that the values will lie on a bell-shaped curve, just as is typical for a large enough sample of examination results.

The demonstrations use two _"score"_ columns, one that holds `double precision` values and one that holds `int` values produced by rounding the `double precision` values with the `round()` function. The demonstrations rely on the fact that the `double precision` scores have no duplicates. This is established by creating a unique index on the _"dp_score"_ column. It's just possible that `normal_rand()` will create some duplicate values. However, this is so very rare that it was never seen as the script was repeated, very many times, during the development of the demonstrations that use this table. If `CREATE INDEX` does fail because of this, just repeat the script by hand. The demonstrations also rely on the fact that the `int` scores will have very many duplicates. This is inevitable when there are only 101 available integers in the scaled range and there are 100,000 rows.

A large value like 100,000 gives the best compromise between the time to populate the table and the effectiveness of the demonstration. These are typical times for 100,000 rows (using YB-2.1.8) on a single-node cluster on a laptop computer:
-   < ~3 sec to populate the table
-   < ~3 sec to create the index

This `ysqlsh` script creates the table _"t4"_ and creates the procedure to populate the table.
Save it as `t4_1.sql`.

```plpgsql
-- Suppress the spurious warning that is raised
-- when the to-be-deleted table doesn't yet exist.
set client_min_messages = warning;
drop table if exists t4 cascade;

create table t4(
  k uuid default gen_random_uuid() primary key,
  dp_score double precision not null,
  int_score int not null);

-- Use normal_rand() to insert the specified number of rows into "t4".
create or replace procedure generate_scores(no_of_rows in int)
  language plpgsql
as $body$
declare
  normal_rand_mean   constant double precision :=  0.0;
  normal_rand_stddev constant double precision := 50.0;

  agg     double precision[]  not null := '{0}';
  min_val double precision    not null := 0;
  max_val double precision    not null := 0;
  scale   double precision    not null := 0;

  zero     constant double precision :=    0;
  hundred  constant double precision := 100;
begin
  with v as (
    select normal_rand(no_of_rows, normal_rand_mean, normal_rand_stddev) as r)
  select array_agg(r)
  into strict agg
  from v;

  with v as (
    select unnest(agg) as u)
  select
    min(u), max(u)
    into strict min_val, max_val
  from v;

  -- Scale the values to the range (zero..hundred)
  -- Score of zero means no-show.
  scale := max_val - min_val;
  insert into t4(dp_score, int_score)
  with v as (
    select (((unnest(agg) - min_val)*hundred)/scale) as u)
  select
    greatest(least(u, hundred), zero), -- protect against rounding errors
    round(u)
  from v;
end;
$body$;
```

This script executes the procedure and then creates a unique index on the _"dp_score"_ column. Save it as `t4_2.sql`.

```plpgsql
-- You can run this script time and again. It will always finish silently.

set client_min_messages = warning;
drop index if exists t4_dp_score_unq;
truncate table t4;

\timing on
call generate_scores(no_of_rows => 100000);
create unique index t4_dp_score_unq on t4(dp_score);
\timing off
```
The section [Analyzing a normal distribution with percent_rank(), cume_dist() and ntile()](../../../analyzing-a-normal-distribution/) shows how to plot a histogram (using plain text in `ysqlsh`). This vividly demonstrates the bell-shaped distribution.
