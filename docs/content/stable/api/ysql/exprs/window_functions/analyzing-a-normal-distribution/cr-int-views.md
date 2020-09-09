---
title: cr_int_views.sql
linkTitle: cr_int_views.sql
headerTitle: cr_int_views.sql
description: cr_int_views.sql - Part of the code kit for the "Analyzing a normal distribution" section within the YSQL window functions documentation.
block_indexing: true
menu:
  stable:
    identifier: cr-int-views
    parent: analyzing-a-normal-distribution
    weight: 50
isTocNested: true
showAsideToc: true
---
Save this script as `cr_int_views.sql`.
```plpgsql
-- Suppress the spurious warning that is raised
-- when the to-be-deleted view doesn't yet exist.
set client_min_messages = warning;
drop view if exists t4_view;

create view t4_view as
select
  k,
  int_score as score
from t4;

-- This very simple view allows updates.
drop view if exists results;
create view results as
select method, bucket, n, min_s, max_s
from int_results;
```
