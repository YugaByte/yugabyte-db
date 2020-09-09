---
title: cr_do_percent_rank.sql
linkTitle: cr_do_percent_rank.sql
headerTitle: cr_do_percent_rank.sql
description: Create the function that creates the histogram output.
block_indexing: true
menu:
  v2.1:
    identifier: cr-do-percent-rank
    parent: analyzing-a-normal-distribution
    weight: 120
isTocNested: true
showAsideToc: true
---
Save this script as `cr_do_percent_rank.sql`.
```postgresql
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
