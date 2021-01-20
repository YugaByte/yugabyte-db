---
title: covar_pop(), covar_samp(), corr(), regr_%()
linkTitle: linear regression
headerTitle: Functions for linear regression analysis
description: Describes the functionality of the covar_pop(), covar_samp(), corr(), and regr_%() family of YSQL aggregate functions for linear regression analysis
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: linear-regression
    parent: aggregate-function-syntax-semantics
    weight: 50
isTocNested: true
showAsideToc: true
---

This parent section and its two child sections describe these aggregate functions for linear regression analysis:

- [`covar_pop()`](./covar-corr/#covar-pop-covar-samp), [`covar_samp()`](./covar-corr/#covar-pop-covar-samp), [`corr()`](./covar-corr/#corr)
- [`regr_avgy()`](./regr/#regr-avgy-regr-avgx), [`regr_avgx()`](./regr/#regr-avgy-regr-avgx), [`regr_count()`](./regr/#regr-count), [`regr_slope()`](./regr/#regr-slope-regr-intercept), [`regr_intercept()`](./regr/#regr-slope-regr-intercept), [`regr_r2()`](./regr/#regr-r2), [`regr_syy()`](./regr/#regr-syy-regr-sxx-regr-sxy)[`regr_sxx()`](./regr/#regr-syy-regr-sxx-regr-sxy), [`regr_sxy()`](./regr/#regr-syy-regr-sxx-regr-sxy).

See, for example, this Wikipedia article on [Regression analysis](https://en.wikipedia.org/wiki/Regression_analysis). Briefly, linear regression analysis estimates the relationship between a dependent variable and an independent variable, aiming to find the line that most closely fits the data. This is why each of the functions described has two input formal parameters. The _dependent variable_, the first formal parameter, is conventionally designated by _"y"_; and the _independent variable_, the second formal parameter, is conventionally designated by _"x"_.

See, for example, the article ["How To Interpret R-squared in Regression Analysis"](https://statisticsbyjim.com/regression/interpret-r-squared-regression/). It says this:

>  Linear regression identifies the equation that produces the smallest difference between all of the observed values and their [fitted values](https://statisticsbyjim.com/glossary/fitted-values/). To be precise, linear regression finds the smallest sum of squared [residuals](https://statisticsbyjim.com/glossary/residuals/) that is possible for the dataset.

In terms of the high school equation for a straight line:

```
y = m*x + c
```

the function [`regr_slope(y, x)`](./regr/#regr-slope-regr-intercept) estimates the gradient, _"m"_,  of the straight line that best fits the set of coordinate pairs over which the aggregation is done; and the function [`regr_intercept(y, x)`](./regr/#regr-slope-regr-intercept) estimates its intercept with the y-axis, _"c"_. The so-called "R-squared " measure, implemented by   [`regr_r2(y, x)`](./regr/#regr-r2), indicates the goodness-of-fit. It measures the percentage of the variance in the dependent variable that the independent variables explain collectively—in other words, the strength of the relationship between your model and the dependent variable on a 0 – 100% scale. For example, if `regr_r2()` returns a value of _0.7_, it means that seventy percent of the relationship between the putative dependent variable and the independent variable can be explained by a straight line with the gradient and intercept returned, respectively, by `regr_slope()` and `regr_intercept()`.  The remaining thirty percent can be attributed to stochastic variation. 

The purpose of each of the functions is rather specialized; but the domain is also very familiar to people who need to do linear regression. For this reason, the aim here is simply to explain enough for specialists to be able to understand exactly what is available, and how to invoke what they decide that they need. Each function is illustrated with a simple example.

Each of these aggregate functions is invoked by using the same syntax:

- _either_ the simple syntax, `select aggregate_fn(expr, expr) from t`
- _or_ the `GROUP BY` syntax
- _or_ the `OVER` syntax

Only the simple invocation is illustrated. See, for example, the sections [`GROUP BY` syntax](../avg-count-max-min-sum/#group-by-syntax) and [`OVER` syntax](../avg-count-max-min-sum/#over-syntax) in the section [`avg(), count(), max(), min(), sum()`](../function-syntax-semantics/avg-count-max-min-sum/) for how to use these syntax patterns.

**Signature:**

Each one of the aggregate functions for linear regression analysis, except for `regr_count()`, has the same signature:

```
input value:       double precision, double precision
                   
return value:      double precision
```

Because it returns a count, `regr_count()` returns a `bigint`, thus:

```
input value:       double precision, double precision
                   
return value:      bigint
```
In all cases, the first input parameter represents the values that you want to be taken as the _dependent variable_ (conventionally denoted by _"y"_) and the second input parameter represents the values that you want to be taken as the _independent variable_ (conventionally denoted by _"x"_).

{{< note title="About nullness" >}}
If, for a particular input row, _either_ the expression for _"y"_, _or_ the expression for _"x"_, evaluates to `null`, then that row is implicitly filtered out.
{{< /note >}}

## Create the test table

The same test table recipe serves for illustrating all of the functions for linear regression analysis. The design is straightforward. Noise is added to a pure linear function, thus:

```
  y = slope*x + intercept + delta
```

where _"delta"_ is picked, for each _"x"_ value from a pseudorandom normal distribution with specified mean and standard deviation.

The procedure _"populate_t()"_ lets you try different values for _"slope"_, _"intercept"_, and for the size and variability of _"delta"_. It uses the function `normal_rand()`, brought by the [tablefunc](../../../extensions/#tablefunc) extension.

```plpgsql
drop procedure if exists populate_t(
  int,  double precision,  double precision,  double precision,  double precision)
  cascade;
drop table if exists t cascade;

create table t(
  k      int primary key,
  x      double precision,
  y      double precision,
  delta  double precision);

create procedure populate_t(
  no_of_rows  in int,
  slope       in double precision,
  intercept   in double precision,
  mean        in double precision,
  stddev      in double precision)
  language plpgsql
as $body$
begin
  delete from t;

  with
    a1 as (
      select
        s.v        as k,
        s.v        as x,
        (s.v * slope) + intercept as y
      from generate_series(1, no_of_rows) as s(v)),

    a2 as (
      select (
        row_number() over()) as k,
        r.v as delta
      from normal_rand(no_of_rows, mean, stddev) as r(v))

  insert into t(k, x, y, delta)
  select
    k, x, a1.y, a2.delta
  from a1 inner join a2 using(k);

  insert into t(k, x, y, delta) values
    (no_of_rows + 1,    0, null, null),
    (no_of_rows + 2, null,    0, null);
end;
$body$;

\set no_of_rows 100
call populate_t(
  no_of_rows  => :no_of_rows,

  mean        =>  0.0,
  stddev      => 20.0,
  slope       =>  5.0,
  intercept   =>  3.0);
  
\pset null <null>
with a as(
  select k, x, y, delta from t where x between 1 and 5
  union all
  select k, x, y, delta from t where k between 96 and (:no_of_rows + 2))
select
  to_char(x, '990.9')           as x,
  to_char(y, '990.9')           as y,
  to_char((y + delta), '990.9999') as "y + delta"
from a
order by k;
```

Here is an impression of the result of invoking  _"populate_t()"_ with the values shown. The whitespace has been manually added.

```
   x    |   y    | y + delta 
--------+--------+-----------
    1.0 |    8.0 |   -5.9595
    2.0 |   13.0 |  -14.8400
    3.0 |   18.0 |   40.4009
    4.0 |   23.0 |   27.8537
    5.0 |   28.0 |   68.7411
  
   96.0 |  483.0 |  483.9196
   97.0 |  488.0 |  464.3205
   98.0 |  493.0 |  528.2446
   99.0 |  498.0 |  514.0421
  100.0 |  503.0 |  549.7692
    0.0 | <null> | <null>
 <null> |    0.0 | <null>
```

The individual functions are described in these two child-sections

- [`covar_pop()`, `covar_samp()`, `corr()`](./covar-corr/)

- [`regr_%()`](./regr/)


