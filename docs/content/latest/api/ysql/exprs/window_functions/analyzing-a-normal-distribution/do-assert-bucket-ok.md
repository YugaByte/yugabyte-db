---
title: do_assert_bucket_ok
linkTitle: do_assert_bucket_ok
headerTitle: do_assert_bucket_ok.sql
description: do_assert_bucket_ok.sql - Part of the code kit for the "Analyzing a normal distribution" section within the YSQL window functions documentation.
menu:
  latest:
    identifier: do-assert-bucket-ok
    parent: analyzing-a-normal-distribution
    weight: 90
isTocNested: true
showAsideToc: true
---
Save this script as `do_assert_bucket_ok.sql`.
```plpgsql
-- Test it over the full range.
-- Pay special attention to the bucket boundaries.
do $body$
declare
  one constant int := 1;
  two constant int := 2;

  lower_bound  constant double precision := 0;
  upper_bound  constant double precision := 100;
  no_of_values constant int              := 10;

  inputs constant double precision[] := array[

                       -- expected_bucket
     0,                --  1
     0.0000000001,     --  1
     0.0000000002,     --  1
    10,                --  1

    10.00000001,  20, --   2
    20.00000001,  30, --   3
    30.00000001,  40, --   4
    40.00000001,  50, --   5
    50.00000001,  60, --   6
    60.00000001,  70, --   7
    70.00000001,  80, --   8
    80.00000001,  90, --   9
    90.00000001, 100  --  10
    ];

  expected_buckets constant int[] := array[1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
begin
  for j in 1..array_upper(inputs, 1) loop
    declare
      expected_bucket constant int :=
        expected_buckets[round((j + one)/two)::int];
    begin
      assert
        bucket(inputs[j], lower_bound, upper_bound, no_of_values) = expected_bucket,
      'assert failed for test no. '||j::text||':
        bucket('||inputs[j]::text||', ...) <> '||expected_bucket::text;
    end;

    -- Provoke the expected assert failures from bucket().
    declare
      result double precision not null := 0;
    begin
      begin
        result := bucket(-0.0000000002, lower_bound, upper_bound, no_of_values);
        raise exception 'Logic error';
      exception
        when assert_failure then
          -- raise info 'caught expected assert_failure';
          null;
      end;
      begin
        result := bucket(100.0000000001, lower_bound, upper_bound, no_of_values);
        raise exception 'Logic error';
      exception
        when assert_failure then
          -- raise info 'caught expected assert_failure';
          null;
      end;
    end;
  end loop;
end;
$body$;
```
