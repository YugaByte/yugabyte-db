---
title: do_populate_results.sql
linkTitle: do_populate_results.sql
headerTitle: do_populate_results.sql
description: do_populate_results.sql - Part of the code kit for the "Analyzing a normal distribution" section within the YSQL window functions documentation.
menu:
  latest:
    identifier: do-populate-results
    parent: analyzing-a-normal-distribution
    weight: 140
isTocNested: true
showAsideToc: true
---
Save this script as `do_populate_results.sql`.
```plpgsql
do $body$
declare
  nof_buckets constant int not null := 20;
begin
  delete from results;
  call do_ntile        (nof_buckets);
  call do_percent_rank (nof_buckets);
  call do_cume_dist    (nof_buckets);
end;
$body$;
```
