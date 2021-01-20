---
title: cr_histogram.sql
linkTitle: cr_histogram.sql
headerTitle: cr_histogram.sql
description: cr_histogram.sql - Part of the code kit for the "Analyzing a normal distribution" section within the YSQL window functions documentation.
menu:
  latest:
    identifier: cr-histogram
    parent: analyzing-a-normal-distribution
    weight: 100
isTocNested: true
showAsideToc: true
---
Save this script as `cr_histogram.sql`.
```plpgsql
-- "scale_factor" controls the histogram height.
-- Use 10 for 10,000 rows, 100 for 100,000 rows.

create or replace function histogram(
  no_of_bukets in int,
  scale_factor in numeric)
  returns SETOF text
  language sql
as $body$
  with
    bucket_assignments as (
      select bucket(dp_score, 0::double precision, 100::double precision, no_of_bukets) as bucket
      from t4)
  select
    to_char(count(*), '9999')||' | '||
    rpad('=', (count(*)/scale_factor)::int, '=')
  from bucket_assignments
  group by bucket
  order by bucket;
$body$;
```
