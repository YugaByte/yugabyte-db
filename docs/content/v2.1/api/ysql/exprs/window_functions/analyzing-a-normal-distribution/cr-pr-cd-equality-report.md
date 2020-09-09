---
title: cr_pr_cd_equality_report.sql
linkTitle: cr_pr_cd_equality_report.sql
headerTitle: cr_pr_cd_equality_report.sql
description: Create a function to compute some basic facts about table t4.
block_indexing: true
menu:
  v2.1:
    identifier: cr-pr-cd-equality-report
    parent: analyzing-a-normal-distribution
    weight: 60
isTocNested: true
showAsideToc: true
---
Save this script as `cr_pr_cd_equality_report.sql`.
```postgresql
set client_min_messages = warning;
drop type if exists pr_cd_equality_report_t cascade;
create type pr_cd_equality_report_t as("count(*)" int, max_score text, max_ratio text);

create or replace function pr_cd_equality_report(
  delta_threshold in double precision)
  returns SETOF pr_cd_equality_report_t
  immutable
  language sql
as $body$
  with
    measures as (
      select
        score,
        (percent_rank() over w) as pr,
        (cume_dist()    over w) as cd
      from t4_view
      window w as (order by score))
    ,
    ratios as (
      select
        score,
        (pr*100::double precision)/cd as ratio
      from measures)
    ,
    deltas as (
      select
        score,
        ratio,
        abs(ratio - 100) as delta
      from ratios)
    ,
    bad_deltas as (
      select
        score,
        ratio,
        delta
      from deltas
      where delta > delta_threshold)
    ,
    result as (
      select
        count(*)                      as n,
        to_char(max(score), '999.99') as max_score,
        to_char(max(ratio), '999.99') as max_ratio
      from bad_deltas)

select (n, max_score, max_ratio)::pr_cd_equality_report_t
from result;
$body$;
```
