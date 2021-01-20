---
title: regr_%()
linkTitle: regr_%()
headerTitle: regr_%()
description: Describes the functionality of the regr_%() YSQL aggregate functions for linear regression analysis
menu:
  latest:
    identifier: regr
    parent: linear-regression
    weight: 20
isTocNested: true
showAsideToc: true
---

This section describes these aggregate functions for linear regression analysis:

- [`regr_avgy()`](./#regr-avgy-regr-avgx), [`regr_avgx()`](./#regr-avgy-regr-avgx), [`regr_count()`](./#regr-count), [`regr_slope()`](./#regr-slope-regr-intercept), [`regr_intercept()`](./#regr-slope-regr-intercept), [`regr_r2()`](./#regr-r2), [`regr_syy()`](./#regr-syy-regr-sxx-regr-sxy), [`regr_sxx()`](./#regr-syy-regr-sxx-regr-sxy), [`regr_sxy()`](./#regr-syy-regr-sxx-regr-sxy).

Make sure that you have read the [Functions for linear regression analysis](../../linear-regression/) parent section before reading this section. You will need the same data that the parent section shows you how to create. You will also need the function `approx_equal()` that was used in the [Checking the formulas for covariance and correlation](../covar-corr/#checking-the-formulas-for-covariance-and-correlation) section. Create it like this.

```plpgsql
drop function if exists approx_equal(double precision, double precision) cascade;
create function approx_equal(
  n1  in double precision,
  n2  in double precision)
  returns boolean
  language sql
as $body$
  select 2.0::double precision*abs(n1 - n2)/(n1 + n2) < (2e-15)::double precision;
$body$;
```

## regr_avgy(), regr_avgx()

**Purpose:** `regr_avgy()` returns the average of the first argument for those rows where both arguments are `NOT NULL`. `regr_avgx()` returns the average of the second argument for those rows where both arguments are `NOT NULL`.  

## regr_count()

**Purpose:** returns the number of rows where both arguments are `NOT NULL`.

## regr_avgy(), regr_avgx(), regr_count() semantics demonstration

Do this:

```plpgsql
create or replace view v as select x, y from t;
create or replace view test as
with a as (
  select
    regr_avgy(y, x)                        as r_avgy,
    regr_avgx(y, x)                        as r_avgx,
    regr_count(y, x)                       as r_count,

    avg(y) filter (where x is not null)    as avgy,
    avg(x) filter (where y is not null)    as avgx,
    count(y) filter (where x is not null)  as county,
    count(x) filter (where y is not null)  as countx
  from v)
select
  approx_equal(r_avgy, avgy)::text     as "r_avgy = avgy",
  approx_equal(r_avgx, avgx)::text     as "r_avgx = avgx",
  approx_equal(r_count, county)::text  as "r_count = county",
  approx_equal(r_count, countx)::text  as "r_count = countx"
from a;
```

Test it like this on the noise-free data:

```plpgsql
create or replace view v as select x, y from t;
select * from test;
```

This is the result:

```
 r_avgy = avgy | r_avgx = avgx | r_count = county | r_count = countx 
---------------+---------------+------------------+------------------
 true          | true          | true             | true
```

Now test it on the noisy data:

```plpgsql
create or replace view v as select x, (y + delta) as y from t;
select * from test;
```

The result is the same.

## regr_slope(), regr_intercept()

**Purpose:** `regr_slope()` returns the slope of the straight line that linear regression analysis has determined best fits the _"(y, x)"_ pairs. And `regr_intercept()` returns the point at which this line intercepts the _"y"_-axis.

Try this:

```plpgsql
select
  to_char(regr_slope(y, x),               '0.99999999')  as "noise-free slope",
  to_char(regr_intercept(y, x),           '0.99999999')  as "noise-free intercept",

  to_char(regr_slope((y + delta), x),     '0.99999999')  as "noisy slope",
  to_char(regr_intercept((y + delta), x), '0.99999999')  as "noisy intercept"
from t;
```

This is a typical result:

```
 noise-free slope | noise-free intercept | noisy slope | noisy intercept 
------------------+----------------------+-------------+-----------------
  5.00000000      |  3.00000000          |  4.99778685 |  3.70820546
```

Recall (see the [Create the test table](../../linear-regression/#create-the-test-table) section) that the table was populated using this parameterization:

```plpgsql
\set no_of_rows 100
call populate_t(
  no_of_rows  => :no_of_rows,

  mean        =>  0.0,
  stddev      => 20.0,
  slope       =>  5.0,
  intercept   =>  3.0);
```

The results from `regr_slope()` and `regr_intercept()` line up well with what you'd expect.

The section [Scatter-plot for synthetic data](../../../covid-data-case-study/analyze-the-covidcast-data/scatter-plot-for-2020-10-21/#scatter-plot-for-synthetic-data) shows a scatter plot of the data in table _"t"_ (created with actual arguments for procedure _"populate_t()"_ that that section specifies) with the straight line whose slope and y-axis intercept, as returned by `regr_slope()` and `regr_intercept()`, superimposed.

## regr_r2()

**Purpose:** Returns the square of the correlation coefficient, [`corr()`](../covar-corr/#corr).

Try this:

```plpgsql
select
  approx_equal(regr_r2(y, x),           corr(y, x)^2          )::text  as "test for noise-free data",
  approx_equal(regr_r2((y + delta), x), corr((y + delta), x)^2)::text  as "test for noisy data"
from t;
```

This is the result:

```
 test for noise-free data | test for noisy data 
--------------------------+---------------------
 true                     | true
```

## regr_syy(), regr_sxx(), regr_sxy()

**Purpose:** These three measures are clearly defined in terms of other statistical aggregate functions described in this overall section. Statisticians will know when they need them.

- `regr_syy(y, x)` returns [`regr_count(y, x)`](./#regr-count)`*`[`var_pop(y)`](../../variance-stddev/#var-pop) for `NOT NULL` pairs.

- `regr_sxx(y, x)` returns [`regr_count(y, x)`](./#regr-count)`*`[`var_pop(x)`](../../variance-stddev/#var-pop) for `NOT NULL` pairs.

- `regr_sxy(y, x)` returns [`regr_count(y, x)`](./#regr-count)`*`[`covar_pop(y, x)`](../covar-corr/#covar-pop-covar-samp) for `NOT NULL` pairs.

## regr_syy(), regr_sxx(), regr_sxy() basic semantics demonstration

Try this first:

```plpgsql
create or replace view v as select x, y from t;
create or replace view test as
with a as (
  select x, y
  from v
  where x is not null and y is not null)
select
  approx_equal( regr_syy(y, x), (regr_count(y, x)*var_pop(y)) )     ::text  as "regr_syy() test",
  approx_equal( regr_sxx(y, x), (regr_count(y, x)*var_pop(x)) )     ::text  as "regr_sxx() test",
  approx_equal( regr_sxy(y, x), (regr_count(y, x)*covar_pop(y, x)) )::text  as "regr_sxy() test"
from a;
```
Now do this to test the semantics on the noise-free data:

```plpgsql
create or replace view v as select x, y from t;
select * from test;
```

This is the result:

```
 regr_syy() test | regr_sxx() test | regr_sxy() test 
-----------------+-----------------+-----------------
 true            | true            | true
```

Now do this to test the semantics on the noisy data:

```plpgsql
create or replace view v as select x, (y + delta) as y from t;
select * from test;
```
The result is the same as the result for the noise-free data.

## Checking the formulas for regr_syy(), regr_sxx(), regr_sxy() 

Now try this:

```plpgsql
create or replace view v as select x, y from t;

drop function if exists f() cascade;
create function f()
  returns table(t text)
  language plpgsql
as $body$
declare
  regr_syy constant double precision not null := (
    select regr_syy(y, x) from v);
  regr_sxx constant double precision not null := (
    select regr_sxx(y, x) from v);
  regr_sxy constant double precision not null := (
    select regr_sxy(y, x) from v);

  count  double precision not null := 0;
  sum_yy double precision not null := 0;
  sum_xx double precision not null := 0;
  sum_xy double precision not null := 0;
  sum_y  double precision not null := 0;
  sum_x  double precision not null := 0;
begin
  with a as (
    select x, y
    from v
    where x is not null and y is not null)
  select count(*), sum(y^2), sum(x^2), sum(x*y), sum(y), sum(x)
  into count, sum_yy, sum_xx, sum_xy, sum_y, sum_x
  from a;

  assert
    approx_equal(regr_syy, (sum_yy - sum_y^2)/count),
  'unexpected';

  assert
    approx_equal(regr_sxx, (sum_xx - sum_x^2)/count),
  'unexpected';

  assert
    approx_equal(regr_sxy, (sum_xy - sum_y*sum_x)/count),
  'unexpected';

  t := 'regr_syy: '||to_char(regr_syy, '99999990.99999999'); return next;
  t := 'regr_sxx: '||to_char(regr_sxx, '99999990.99999999'); return next;
  t := 'regr_sxy: '||to_char(regr_sxy, '99999990.99999999'); return next;
end;
$body$;

create or replace view v as select x, y from t;
select t as "noise-free data" from f();

create or replace view v as select x, (y + delta) as y from t;
select t as "noisy data" from f();
```

This is a typical result:

```
       noise-free data        
------------------------------
 regr_syy:   2083125.00000000
 regr_sxx:     83325.00000000
 regr_sxy:    416625.00000000

          noisy data          
------------------------------
 regr_syy:   2121737.65951556
 regr_sxx:     83325.00000000
 regr_sxy:    416440.58954855
```
