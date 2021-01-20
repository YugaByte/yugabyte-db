---
title: cr_bucket_using_width_bucket.sql
linkTitle: cr_bucket_using_width_bucket.sql
headerTitle: cr_bucket_using_width_bucket.sql
description: cr_bucket_using_width_bucket.sql - Part of the code kit for the "Analyzing a normal distribution" section within the YSQL window functions documentation.
menu:
  latest:
    identifier: cr-bucket-using-width-bucket
    parent: analyzing-a-normal-distribution
    weight: 70
isTocNested: true
showAsideToc: true
---
Save this script as `cr_bucket_using_width_bucket.sql`.
```plpgsql
-- This approach subtracts a tiny value, epsilon, from the input value
-- to change "width_bucket()"'s "closed-open" interval bucket semantics to
-- the required "open-closed" interval bucket semantics.
--
-- You might consider this to be a rather obscure and possibly risky trick.
-- However, this implementation of "bucket()" does pass the rigorous
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
  declare
    one     constant int              := 1;
    zero    constant double precision := 0;
    epsilon constant double precision := 0.0000000001;

    result constant int not null :=
      case
        when val between zero and epsilon
          then one
        else
          width_bucket((val - epsilon), lower_bound, upper_bound, no_of_values)
      end;
  begin
    assert
      (result between one and no_of_values),
    'bucket():'||
    ' val '||val||
    ' must be between lower_bound '||lower_bound||
    ' and upper_bound '||upper_bound;

    return result;
  end;
end;
$body$;
```
