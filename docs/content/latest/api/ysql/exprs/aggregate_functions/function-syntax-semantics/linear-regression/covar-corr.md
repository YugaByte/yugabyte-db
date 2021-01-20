---
title: covar_pop(), covar_samp(), corr()
linkTitle: covar_pop(), covar_samp(), corr()
headerTitle: covar_pop(), covar_samp(), corr()
description: Describes the functionality of the covar_pop(), covar_samp(), and corr() YSQL aggregate functions for linear regression analysis
menu:
  latest:
    identifier: covar-corr
    parent: linear-regression
    weight: 10
isTocNested: true
showAsideToc: true
---

This section describes these aggregate functions for linear regression analysis:

- [`covar_pop()`](./#covar-pop-covar-samp), [`covar_samp()`](./#covar-pop-covar-samp), [`corr()`](./#corr)

Make sure that you have read the [Functions for linear regression analysis](../../linear-regression/) parent section before reading this section. You will need the same data that the parent section shows you how to create. 

## covar_pop(), covar_samp()

**Purpose:** Returns the so-called covariance, either taking the available values to be the _entire population_ or taking them to be a _sample_ of the population. This distinction is explained in the section [`variance()`, `var_pop()`, `var_samp()`, `stddev()`, `stddev_pop()`, `stddev_samp()`](../variance-stddev/).

These measures are explained in the Wikipedia article [Covariance](https://en.wikipedia.org/wiki/Covariance#Definition). It says this (bolding added in the present documentation):

> covariance is a measure of the joint variability of two random variables. If the greater values of one variable mainly correspond with the greater values of the other variable, and the same holds for the lesser values, (i.e., the variables tend to show similar behavior), the covariance is positive. In the opposite case, when the greater values of one variable mainly correspond to the lesser values of the other, (i.e., the variables tend to show opposite behavior), the covariance is negative. The sign of the covariance therefore shows the tendency in the linear relationship between the variables. The magnitude of the covariance is not easy to interpret because it is not normalized and hence depends on the magnitudes of the variables. **The normalized version of the covariance, the correlation coefficient, however, shows by its magnitude the strength of the linear relation**.

Try this:

```plpgsql
select
  to_char(covar_pop(y, x),           '9990.9999')  as "covar_pop(y, x)",
  to_char(covar_pop((y + delta), x), '9990.9999')  as "covar_pop((y + delta), x)"
from t;
```

This is a typical result:

```
 covar_pop(y, x) | covar_pop((y + delta), x) 
-----------------+---------------------------
  4166.2500      |  4164.4059
```

As promised, it is not easy to interpret the magnitude of these values.

The article gives the formulas for computing the measures. The section [Checking the formulas for covariance and correlation](./#checking-the-formulas-for-covariance-and-correlation) shows code that compares the results produced by the built-in aggregate functions with results produced by using the explicit formulas.

The formulas show (by virtue of the commutative property of multiplication) that the ordering of the columns that you use as the actual arguments for the first and second input parameters is of no consequence.

## corr()

**Purpose:** Returns the so-called correlation coefficient. This measures the extent to which the _"y"_ values are linearly related to the _"x"_ values. A return value of _1.0_ indicates perfect correlation.

This measure is explained in the Wikipedia article [Correlation and dependence](https://en.wikipedia.org/wiki/Correlation_and_dependence#Definition). Briefly, it's the normalized version of the covariance. The article gives the formula for computing the measure. The section [Checking the formulas for covariance and correlation](./#checking-the-formulas-for-covariance-and-correlation) shows code that compares the result produced by the built-in aggregate function with the result produced by using the explicit formula.

The formula show (by virtue of the commutative property of multiplication) that the ordering of the columns that you use as the actual arguments for the first and second input parameters is of no consequence.

The values of _"t.y"_ have been created to be perfectly correlated with those of "_t.x"_. And the values of _"(y + delta)"_ have been created to be noisily correlated. Try this:

```plpgsql
select
  to_char(corr(y, x),           '90.9999')  as "corr(y, x)",
  to_char(corr((y + delta), x), '90.9999')  as "corr((y + delta), x)"
from t;
```

This is a typical result:

```
 corr(y, x) | corr((y + delta), x) 
------------+----------------------
   1.0000   |   0.9904
```

The noisy value pairs, _"x"_ and _"(y + delta)"_, are less correlated than the noise-free value pairs, _"x"_ and _"y"_.

## Checking the formulas for covariance and correlation

{{< note title="Excluding rows where either y is NULL or x is NULL" >}}
Notice that explicit code is needed in the functions that implement the formulas. The invocations of the aggregate functions that take a single argument, _"y"_ or _"x"_, must mimic the semantics of the two-argument linear regression aggregate functions by using a `FILTER` clause to exclude rows where the other column (respectively _"x"_ or _"y"_) is `null`.
{{< /note >}}

First, create a view, _"v"_, to expose the noise-free data in table _"t"_:

```plpgsql
create or replace view v as select x, y from t;
```

Now create a function to calculate the effect of these two aggregate functions:

```plpgsql
select covar_pop(y, x) from v;
```

and

```plpgsql
select covar_samp(y, x) from v;
```

The function _"covar_formula_y_x(mode in text)"_ implements both formulas.

```plpgsql
drop function if exists covar_formula_y_x(text) cascade;
create function covar_formula_y_x(mode in text)
  returns double precision
  language plpgsql
as $body$
declare
  count constant double precision not null := (
    select count(*) filter (where y is not null and x is not null) from v);

  avg_y constant double precision not null := (
    select avg(y) filter (where x is not null) from v);
  avg_x constant double precision not null := (
    select avg(x) filter (where y is not null) from v);

  -- The "not null" constraint traps "case not found".
  covar_formula_y_x constant double precision not null :=
    case lower(mode)
      when 'pop' then
        (select sum((y - avg_y)*(x - avg_x)) from v)/count
      when 'samp' then
        (select sum((y - avg_y)*(x - avg_x)) from v)/(count - 1)
    end;
begin
  return covar_formula_y_x;
end;
$body$;
```

The calculations differ only in that the divisor is _"N"_ (the number of values) for the "population" variant and is is _"(N - 1)"_ for the "sample" variant—symmetrically with the way that [`var_pop()`](../../variance-stddev/#var-pop) and [`stddev_pop()`](../../variance-stddev/#stddev-pop) differ from [`var_samp()`](../../variance-stddev/#var-samp) and [`stddev_samp()`](../../variance-stddev/#stddev-samp).

Next, create the function _"corr_formula_y_x()"_ to the effect of this aggregate function:

```plpgsql
select corr(y, x) from v;
```

This aggregate function has just a single variant:

```plpgsql
drop function if exists corr_formula_y_x() cascade;
create function corr_formula_y_x()
  returns double precision
  language plpgsql
as $body$
declare
  count constant double precision not null := (
    select count(*) filter (where y is not null and x is not null) from v);

  avg_y constant double precision not null := (
    select avg(y) filter (where x is not null) from v);
  avg_x constant double precision not null := (
    select avg(x) filter (where y is not null) from v);

  stddev_y constant double precision not null := (
    select stddev(y) filter (where x is not null) from v);
  stddev_x constant double precision not null := (
    select stddev(x) filter (where y is not null) from v);

  syx constant double precision not null := (
    select sum((y - avg_y)*(x - avg_x)) from v)/(stddev_y*stddev_x);

  corr_formula_y_x constant double precision not null :=
    syx/(count - 1);
begin
  return corr_formula_y_x;
end;
$body$;
```

The aim is now to check:

- _both_ the commutative property of `covar_pop(y, x)` and `covar_samp(y, x)` with respect to the arguments _"x"_ and _"y"_

- _and_ that the formulas return the same values as the built-in aggregate functions.

This requires an equality test. However, because the return values are not integers, the test must use a tolerance. `double precision` arithmetic takes advantage of IEEE-conformant 16-bit hardware implementation, adding just a little extra software implementation to accommodate the possibility of `null`s. The Wikipedia article [Double-precision floating-point format](https://en.wikipedia.org/wiki/Double-precision_floating-point_format) says that the precision is, at worst, 15 significant decimal digits precision. This means that the accuracy of a `double precision` computation that combines several values using addition, subtraction, multiplication, and division can be worse than 15 significant decimal digits. Empirical testing showed that all the necessary comparisons typically passed the test that the function _"approx_equal()"_ implements. Notice that it uses a tolerance of _"(2e-15)"_.

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

If the test fails (as it is bound, occasionally to do), just recreate the table. Because of the pseudorandom nature of `normal_rand()`, it's very likely that the test will now succeed.

Finally, create a function to execute the tests and to show the interesting distinct outcomes:

```plpgsql
drop function if exists f() cascade;
create function f()
  returns table(t text)
  language plpgsql
as $body$
declare
  covar_pop_y_x      constant double precision not null := (select covar_pop(y, x) from v);
  covar_pop_x_y      constant double precision not null := (select covar_pop(x, y) from v);

  covar_samp_y_x     constant double precision not null := (select covar_samp(y, x) from v);
  covar_samp_x_y     constant double precision not null := (select covar_samp(x, y) from v);

  corr_y_x           constant double precision not null := (select corr(y, x) from v);
  corr_x_y           constant double precision not null := (select corr(x, y) from v);
begin
  assert approx_equal(covar_pop_y_x, covar_pop_x_y),              'unexpected 1';
  assert approx_equal(covar_formula_y_x('pop'), covar_pop_y_x),   'unexpected 2';

  assert approx_equal(covar_samp_y_x, covar_samp_x_y),            'unexpected 3';
  assert approx_equal(covar_formula_y_x('samp'), covar_samp_y_x), 'unexpected 4';

  assert approx_equal(corr_y_x, corr_x_y),                        'unexpected 5';
  assert approx_equal(corr_formula_y_x(), corr_y_x),              'unexpected 6';

  t := 'covar_pop_y_x:  '||to_char(covar_pop_y_x,  '99990.99999999'); return next;
  t := 'covar_samp_y_x: '||to_char(covar_samp_y_x, '99990.99999999'); return next;
  t := 'corr_y_x:       '||to_char(corr_y_x,       '99990.99999999'); return next;
end;
$body$;
```

Now, perform the test using first the noise-free data and then the noisy data:

```plpgsql
create or replace view v as select x, y from t;
select t as "noise-free data" from f();

create or replace view v as select x, (y + delta) as y from t;
select t as "noisy data" from f();
```

This is a typical result:

```
         noise-free data         
---------------------------------
 covar_pop_y_x:    4166.25000000
 covar_samp_y_x:   4208.33333333
 corr_y_x:            1.00000000

           noisy data            
---------------------------------
 covar_pop_y_x:    4164.40589549
 covar_samp_y_x:   4206.47060150
 corr_y_x:            0.99042034
```




