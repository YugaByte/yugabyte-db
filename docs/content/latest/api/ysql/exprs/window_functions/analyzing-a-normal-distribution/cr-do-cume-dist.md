---
title: cr_do_cume_dist.sql
linkTitle: cr_do_cume_dist.sql
headerTitle: cr_do_cume_dist.sql
description: cr_do_cume_dist.sql - Part of the code kit for the "Analyzing a normal distribution" section within the YSQL window functions documentation.
menu:
  latest:
    identifier: cr-do-cume-dist
    parent: analyzing-a-normal-distribution
    weight: 130
isTocNested: true
showAsideToc: true
---
Save this script as `cr_do_cume_dist.sql`.
```plpgsql
create or replace procedure do_cume_dist(no_of_buckets in int)
  language sql
as $body$
  insert into results(method, bucket, n, min_s, max_s)
  with
    measures as (
      select
        score,
        (cume_dist() over w) as measure
      from t4_view
      window w as (order by score))
    ,
    bucket as (
      select
        bucket(measure::numeric, 0::numeric, 1::numeric, $1) as bucket,
        score
      from measures)

  select
    'cume_dist',
    bucket,
    count(*),
    min(score),
    max(score)
  from bucket
  group by bucket;
$body$;
```
