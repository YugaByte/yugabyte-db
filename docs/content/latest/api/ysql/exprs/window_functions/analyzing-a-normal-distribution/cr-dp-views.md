---
title: cr_dp_views.sql
linkTitle: cr_dp_views.sql
headerTitle: cr_dp_views.sql
description: cr_dp_views.sql - Part of the code kit for the "Analyzing a normal distribution" section within the YSQL window functions documentation.
menu:
  latest:
    identifier: cr-dp-views
    parent: analyzing-a-normal-distribution
    weight: 40
isTocNested: true
showAsideToc: true
---
Save this script as `cr_dp_views.sql`.
```plpgsql
create or replace view t4_view as
select
  k,
  dp_score as score
from t4;

-- This very simple view allows updates.
create or replace view results as
select method, bucket, n, min_s, max_s
from dp_results;
```
