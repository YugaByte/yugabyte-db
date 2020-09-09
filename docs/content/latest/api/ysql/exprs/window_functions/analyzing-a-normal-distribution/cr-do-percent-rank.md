---
title: cr_do_percent_rank.sql
linkTitle: cr_do_percent_rank.sql
headerTitle: cr_do_percent_rank.sql
description: cr_do_percent_rank.sql - Part of the code kit for the "Analyzing a normal distribution" section within the YSQL window functions documentation.
menu:
  latest:
    identifier: cr-do-percent-rank
    parent: analyzing-a-normal-distribution
    weight: 120
isTocNested: true
showAsideToc: true
---
Save this script as `cr_do_percent_rank.sql`.
```plpgsql
create or replace procedure do_percent_rank(no_of_buckets in int)
  language sql
as $body$
  insert into results(method, bucket, n, min_s, max_s)
  with
    measures as (
      select
        score,
        (percent_rank() over w) as measure
      from t4_view
      window w as (order by score))
    ,
    bucket as (
      select
        bucket(measure::numeric, 0::numeric, 1::numeric, $1) as bucket,
        score
      from measures)

  select
    'percent_rank',
    bucket,
    count(*),
    min(score),
    max(score)
  from bucket
  group by bucket;
$body$;
```
