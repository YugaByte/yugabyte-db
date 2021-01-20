---
title: synthetic-data.sql
linkTitle: synthetic-data.sql
headerTitle: Linear regression analysis scatter-plot on synthetic data
description: Show straight line computer by linear regression analysis the best fits a synthetically noisy straight line
menu:
  latest:
    identifier: synthetic-data-sql
    parent: analysis-scripts
    weight: 20
isTocNested: true
showAsideToc: true
---

**Save this script as "synthetic-data.sql"**

```plpgsql
drop procedure if exists populate_t(
  int,  double precision,  double precision,  double precision,  double precision)
  cascade;

drop table if exists t cascade;
create table t(
  k      int primary key,
  x      double precision,
  y      double precision,
  delta  double precision);

create procedure populate_t(
  no_of_rows  in int,
  slope       in double precision,
  intercept   in double precision,
  mean        in double precision,
  stddev      in double precision)
  language plpgsql
as $body$
begin
  delete from t;

  with
    a1 as (
      select
        s.v        as k,
        s.v        as x,
        (s.v * slope) + intercept as y
      from generate_series(1, no_of_rows) as s(v)),

    a2 as (
      select (
        row_number() over()) as k,
        r.v as delta
      from normal_rand(no_of_rows, mean, stddev) as r(v))

  insert into t(k, x, y, delta)
  select
    k, x, a1.y, a2.delta
  from a1 inner join a2 using(k);

  insert into t(k, x, y, delta) values
    (no_of_rows + 1,    0, null, null),
    (no_of_rows + 2, null,    0, null);
end;
$body$;

call populate_t(
  no_of_rows  => 100,
  mean        =>  0.0,
  stddev      => 5.0,
  slope       =>  -1.2,
  intercept   =>  131.4);

\o analysis-results/synthetic-data.txt
with a as (
  select
    regr_r2       ((y + delta), x) as r2,
    regr_slope    ((y + delta), x) as s,
    regr_intercept((y + delta), x) as i
  from t)
select
  to_char(r2, '0.99') as r2,
  to_char(s,  '90.9') as s,
  to_char(i, '990.9') as i
from a;
\o

\t on
\o analysis-results/synthetic-data.csv
select
  round(x)::text||','||round(y + delta)::text
from t
where
  x > 60        and
  x < 95        and
  x is not null and
  y is not null
order by x;
\o
\t off
```
