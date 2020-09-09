---
title: cr_int_views.sql
linkTitle: cr_int_views.sql
headerTitle: cr_int_views.sql
description: Create a function to compute some basic facts about table t4.
block_indexing: true
menu:
  v2.1:
    identifier: cr-int-views
    parent: analyzing-a-normal-distribution
    weight: 50
isTocNested: true
showAsideToc: true
---
Save this script as `cr_int_views.sql`.
```postgresql
-- "create or replace view" not yet supported
do $body$
begin
  begin
    execute 'drop view t4_view';
  exception
    when undefined_table then null;
  end;

  begin
    execute 'drop view results';
  exception
    when undefined_table then null;
  end;
end;
$body$;

create view t4_view as
select
  k,
  int_score as score
from t4;

-- This very simple view allows updates.
create view results as
select method, bucket, n, min_s, max_s
from int_results;
```
