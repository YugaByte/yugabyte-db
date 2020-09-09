---
title: cr_do_cume_dist.sql
linkTitle: cr_do_cume_dist.sql
headerTitle: cr_do_cume_dist.sql
description: Create the function that creates the histogram output.
block_indexing: true
menu:
  v2.1:
    identifier: cr-do-cume-dist
    parent: analyzing-a-normal-distribution
    weight: 130
isTocNested: true
showAsideToc: true
---
Save this script as `cr_do_cume_dist.sql`.
```postgresql
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
