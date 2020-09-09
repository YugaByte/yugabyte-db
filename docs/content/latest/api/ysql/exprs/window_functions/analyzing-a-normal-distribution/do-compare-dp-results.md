---
title: do_compare_dp_results.sql
linkTitle: do_compare_dp_results.sql
headerTitle: do_compare_dp_results.sql
description: do_compare_dp_results.sql - Part of the code kit for the "Analyzing a normal distribution" section within the YSQL window functions documentation.
menu:
  latest:
    identifier: do-compare-dp-results
    parent: analyzing-a-normal-distribution
    weight: 160
isTocNested: true
showAsideToc: true
---
Save this script as `do_compare_dp_results.sql`.
```plpgsql
with
  nt_results as (
    select
    bucket, n, min_s, max_s
    from dp_results
    where method = 'ntile')
  ,
  pr_results as (
    select
    bucket, n, min_s, max_s
    from dp_results
    where method = 'percent_rank')
  ,
  cd_results as (
    select
    bucket, n, min_s, max_s
    from dp_results
    where method = 'cume_dist')

select
  bucket,
  (nt.n = pr.n        )::text as "pr n equal",
  (nt.min_s = pr.min_s)::text as "pr min_s equal",
  (nt.max_s = pr.max_s)::text as "pr max_s equal",
  (nt.n = cd.n        )::text as "cd n equal",
  (nt.min_s = cd.min_s)::text as "cd min_s equal",
  (nt.max_s = cd.max_s)::text as "cd max_s equal"
from nt_results nt
inner join pr_results pr using (bucket)
inner join cd_results cd using (bucket)
order by bucket;
```
