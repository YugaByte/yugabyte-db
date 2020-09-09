---
title: do_report_results.sql
linkTitle: do_report_results.sql
headerTitle: do_report_results.sql
description: do_report_results.sql - Create the function that creates the histogram output.
menu:
  latest:
    identifier: do-report-results
    parent: analyzing-a-normal-distribution
    weight: 150
isTocNested: true
showAsideToc: true
---
Save this script as `do_report_results.sql`.
```plpgsql
\t on
select 'Using ntile().';
\t off

select
  bucket,
  n,
  to_char(min_s, '990.99999') as min_s,
  to_char(max_s, '990.99999') as max_s
from results
where method = 'ntile'
order by bucket;

\t on
select 'Using percent_rank().';
select
  bucket,
  n,
  to_char(min_s, '990.99999') as min_s,
  to_char(max_s, '990.99999') as max_s
from results
where method = 'percent_rank'
order by bucket;

select 'Using cume_dist().';
select
  bucket,
  n,
  to_char(min_s, '990.99999') as min_s,
  to_char(max_s, '990.99999') as max_s
from results
where method = 'cume_dist'
order by bucket;
\t off
```
