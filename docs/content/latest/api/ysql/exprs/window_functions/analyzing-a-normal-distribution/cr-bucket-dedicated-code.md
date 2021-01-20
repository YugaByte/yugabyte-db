---
title: cr_bucket_dedicated_code.sql
linkTitle: cr_bucket_dedicated_code.sql
headerTitle: cr_bucket_dedicated_code.sql
description: cr_bucket_dedicated_code.sql - Part of the code kit for the "Analyzing a normal distribution" section within the YSQL window functions documentation.
menu:
  latest:
    identifier: cr-bucket-dedicated-code
    parent: analyzing-a-normal-distribution
    weight: 80
isTocNested: true
showAsideToc: true
---
Save this script as `cr_bucket_dedicated_code.sql`.
```plpgsql
-- This approach implements the required "open-closed" interval
-- bucket semantics directly. See the test
--
--   where scaled_val > lb and scaled_val <= ub
--
-- at the end.
--
-- You might consider this to be the better approach,  even though
-- it means saying "No thank you" to the free gift of the built-in
-- "width_bucket()" which -- acutally implements the wrong semantics
-- for the present use case. Fixing it up with a trick might feel to be
-- too offensive.
--
-- This implementation of "bucket()" also passes the rigorous
-- acceptance test.

create or replace function bucket(
  val          in double precision,
  lower_bound  in double precision default 0,
  upper_bound  in double precision default 1,
  no_of_values in int              default 10)
  returns int
  language plpgsql
as $body$
begin
  assert
    (val between lower_bound and upper_bound),
    'bucket():'||
    ' val '||val||
    ' must be between lower_bound '||lower_bound||
    ' and upper_bound '||upper_bound;

  declare
    one  constant double precision := 1;
    n    constant double precision := no_of_values;
    scaled_val constant double precision :=
      (val - lower_bound)/(upper_bound - lower_bound);
  begin
    return (
      with
        series as (
          select generate_series::int as s
          from generate_series(1::int, no_of_values))
        ,
        buckets as (
          select
            s                                             as bucket,
            -- (val = 0) is defined to be in (bucket = 1)
            case s when 1 then
              -one
            else
              ((s::double precision) - one)/n
            end                                           as lb,
            (s::double precision)/n                       as ub
          from series)

      select bucket
      from buckets
      where scaled_val > lb and scaled_val <= ub
      );
  end;
end;
$body$;
```
