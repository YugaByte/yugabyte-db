---
title: cr_show_t4.sql
linkTitle: cr_show_t4.sql
headerTitle: cr_show_t4.sql
description: cr_show_t4.sql - Part of the code kit for the "Analyzing a normal distribution" section within the YSQL window functions documentation.
menu:
  latest:
    identifier: cr-show-t4
    parent: analyzing-a-normal-distribution
    weight: 30
isTocNested: true
showAsideToc: true
---
Save this script as `cr_show_t4.sql`.
```plpgsql
-- Function to report on some useful overall measures of t4.
create or replace function show_t4()
  returns table(t varchar)
  language plpgsql
as $body$
declare
  count_star constant int not null :=
    (select count(*) from t4);
  min_dp_score constant numeric not null :=
    (select min(dp_score) from t4);
  max_dp_score constant numeric not null :=
    (select max(dp_score) from t4);
  avg_dp_score constant numeric not null :=
    (select avg(dp_score) from t4);
  dev_dp_score constant numeric not null :=
    (select stddev(dp_score) from t4);

  min_int_score constant numeric not null :=
    (select min(int_score) from t4);
  max_int_score constant numeric not null :=
    (select max(int_score) from t4);
  avg_int_score constant numeric not null :=
    (select avg(int_score) from t4);
  dev_int_score constant numeric not null :=
    (select stddev(int_score) from t4);
begin
  assert
    (min_int_score = 0)                                           and
    (min_int_score::numeric = min_dp_score)                       and
    (max_int_score = 100)                                         and
    (max_int_score::numeric = max_dp_score)                       and
    ((avg_int_score*100.0)/avg_dp_score between 99.99 and 101.01) and
    ((dev_int_score*100.0)/dev_dp_score between 99.99 and 101.01) ,
  'unexpected';

  t := rpad('count(*)', 30)||
       to_char(count_star, '999999999'); return next;

  t := ''; return next;

  t := rpad('avg(%score)', 30)||
       to_char(avg_dp_score, '9999999.9'); return next;

  t := rpad('stddev(%score)', 30)||
       to_char(dev_dp_score, '9999999.9'); return next;
end;
$body$;
```
