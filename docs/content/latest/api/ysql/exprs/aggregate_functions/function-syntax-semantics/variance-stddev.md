---
title: variance(), var_pop(), var_samp(), stddev(), stddev_pop(), stddev_samp()
linkTitle: variance(), var_pop(), var_samp(), stddev(), stddev_pop(), stddev_samp()
headerTitle: variance(), var_pop(), var_samp(), stddev(), stddev_pop(), stddev_samp()
description: Describes the functionality of the variance(), var_pop(), var_samp(), stddev(), stddev_pop(), and stddev_samp() YSQL aggregate functions
menu:
  latest:
    identifier: variance-stddev
    parent: aggregate-function-syntax-semantics
    weight: 40
isTocNested: true
showAsideToc: true
---

This section describes the [`variance()`](./#variance), [`var_pop()`](./#var-pop),  [`var_samp()`](./#var-samp), [`stddev()`](./#stddev), [`stddev_pop()`](./#stddev-pop), and [`stddev_samp()`](./#stddev-samp) aggregate functions. They provide a confidence measure for the computed arithmetic mean of a set of values.

Each of these aggregate functions is invoked by using the same syntax:

- _either_ the simple syntax, `select aggregate_fun(expr) from t`
- _or_ the `GROUP BY` syntax
- _or_ the `OVER` syntax

Only the simple invocation is illustrated in this section. See, for example, the sections [`GROUP BY` syntax](../avg-count-max-min-sum/#group-by-syntax) and [`OVER` syntax](../avg-count-max-min-sum/#over-syntax) in the section [`avg(), count(), max(), min(), sum()`](../function-syntax-semantics/avg-count-max-min-sum/) for how to use these syntax patterns.

## Background

The notions "variance" and "standard deviation" are trivially related: the latter is the square root of the former. The variance of a set of _N_ values, _v_, is defined, naïvely, in terms of the arithmetic mean, _a_ of those values:

```
  variance = ( sum over all "v" of (v - a)^2 ) / N
```

Statisticians distinguish between the variance and the standard deviation of an _entire population_ and the variance and the standard deviation of a _sample_ of a population. The formulas for computing the "population" variants use the naïve definition of variance. And the formulas for computing the "sample" variants divide by _(N - 1)_ rather than by _N_.

This example demonstrates that the built-in functions for the  "population" and the "sample" variants of variance and standard deviation produce the same values as the text-book formulas that define them. First create a small set of values:

```plpgsql
drop table if exists t cascade;
create table t(v numeric primary key);

insert into t(v)
select 100 + s.v*0.01
from generate_series (-5, 5) as s(v);

select to_char(v, '999.99') as v from t order by v;
```

This is the the result:

```
    v    
---------
   99.95
   99.96
   99.97
   99.98
   99.99
  100.00
  100.01
  100.02
  100.03
  100.04
  100.05
```
Now create a function to test the equality between what the built-in functions produce and what the formulas that define them produce:

```plpgsql
drop function if exists fmt(x in numeric) cascade;
drop function if exists f() cascade;

create function fmt(x in numeric)
  returns text
  language sql
as $body$
  select to_char(x, '0.99999999');
$body$;

create function f()
  returns table(t text)
  language plpgsql
as $body$
declare
  sum constant numeric not null := (
    select count(v)::numeric from t);

  avg constant numeric not null := (
    select avg(v) from t);

  s constant numeric not null := (
    select sum((avg - v)^2) from t);

  variance    numeric not null := 0;
  var_samp    numeric not null := 0;
  var_pop     numeric not null := 0;
  stddev      numeric not null := 0;
  stddev_samp numeric not null := 0;
  stddev_pop  numeric not null := 0;
begin
  select variance(v), var_samp(v), var_pop(v), stddev(v), stddev_samp(v), stddev_pop(v)
  into   variance,    var_samp,    var_pop,    stddev,    stddev_samp,    stddev_pop
  from t;

  assert variance = var_samp,              'unexpected';
  assert stddev   = stddev_samp,           'unexpected';

  assert var_samp = s/(sum - 1),           'unexpected';
  assert var_pop  = s/sum,                 'unexpected';

  assert stddev_samp = sqrt(s/(sum - 1)),  'unexpected';
  assert stddev_pop = sqrt(s/sum),         'unexpected';

  t = 'var_samp:               '||fmt(var_samp);                return next;
  t = 'var_pop:                '||fmt(var_pop);                 return next;
  t = 'stddev_samp:            '||fmt(stddev_samp);             return next;
  t = 'stddev_pop:             '||fmt(stddev_pop);              return next;
  t = 'stddev_samp/stddev_pop: '||fmt(stddev_samp/stddev_pop);  return next;
end;
$body$;

\t on
select t from f();
\t off
```

Notice that the semantics of `variance()` and `var_samp()` are identical; and that the semantics of `stddev()` and `stddev_samp()` are identical. Each of the `assert` statements succeeds and the function produces this result:

```
 var_samp:                0.00110000
 var_pop:                 0.00100000
 stddev_samp:             0.03316625
 stddev_pop:              0.03162278
 stddev_samp/stddev_pop:  1.04880885
```

This section assumes that you understand the distinction between the  "population" and the "sample" variants and that you know which variant you need for your present purpose.

**Signature:**

Each one of the "confidence measure" aggregate functions has the same signature:

```
input value:       smallint, int, bigint, numeric, double precision, real
                   
return value:      numeric, double precision
```

**Notes:** The lists of input and return data types give the distinct kinds. Because, the output of each function is computed by division, the return data type is never one whose values are constrained to be whole numbers. Here are the specific mappings:

```
INPUT             OUTPUT
----------------  ----------------
smallint          numeric
int               numeric
bigint            numeric
numeric           numeric
double precision  double precision
real              double precision
```
## variance()

**Purpose:** the semantics of `variance()` and [`var_samp()`](./#var-samp) are identical.

## var_pop()

**Purpose:** Returns the variance of a set of values using the naïve formula (i.e. the "population" variant) that divides by the number of values, _N_, as explained in the [Background](./#background) section. In other words, it treats the set of values as the _entire population_ of interest.

## var_samp()

**Purpose:** Returns the variance of a set of values using the "sample" variant of the formula that divides by _(N - 1)_ where _N_ is the number of values, as explained in the [Background](./#background) section. In other words, it treats the set of values as just a _sample_ of the entire population of interest. The value produced by `var_samp()` is bigger than that produced by `var_pop()`, reflecting the fact that using only a sample is less reliable than using the entire population.

## stddev()

**Purpose:** the semantics of `stddev()` and [`stddev_samp()`](./#stddev-samp) are identical.

## stddev_pop()

**Purpose:** Returns the standard deviation of a set of values using the naïve formula (i.e. the "population" variant) that divides by the number of values, _N_, as explained in the [Background](./#background) section. In other words, it treats the set of values as the _entire population_ of interest.

## stddev_samp()

**Purpose:** Returns the standard deviation of a set of values using the "sample" variant of the formula that divides by _(N - 1)_ where _N_ is the number of values, as explained in the [Background](./#background) section. In other words, it treats the set of values as just a _sample_ of the entire population of interest. The value produced by `stddev_samp()` is bigger than that produced by stddev_pop()`, reflecting the fact that using only a sample is less reliable than using the entire population.

## Example

The example uses the function `normal_rand()`, brought by the [tablefunc](../../../extensions/#tablefunc) extension, to populate the test table:

```plpgsql
drop table if exists t cascade;
create table t(v double precision primary key);

do $body$
declare
  no_of_rows constant int              := 100000;
  mean       constant double precision := 0.0;
  stddev     constant double precision := 50.0;
begin
  insert into t(v)
  select normal_rand(no_of_rows, mean, stddev);
end;
$body$;
```
Of course, the larger is the value that you choose for _"no_of_rows"_, the closer will be the values returned by the "sample" variants of the confidence measures to the values returned by the "population" variants. 

Because the demonstration (for convenience) uses a table with a single `double precision` column, _"v"_,  this must be the primary key. It's just possible that `normal_rand()` will create some duplicate values. However, this is so very rare that it was never seen while the script was repeated, many times, during the development of this code example. If `insert into t(v)` does fail because of this, just repeat the script by hand.

Now display the values for `avg(v)`, `stddev_samp(v)`, `stddev_pop(v)`, and the value of `stddev_samp(v)/stddev_pop(v)`.

```plpgsql
with a as (
  select
    avg(v)         as avg,
    stddev_samp(v) as stddev_samp,
    stddev_pop(v)  as stddev_pop
  from t)
select
  to_char(avg,              '0.999') as avg,
  to_char(stddev_samp, '999.999999') as stddev_samp,
  to_char(stddev_pop,  '999.999999') as stddev_pop,

  to_char(stddev_samp/stddev_pop, '90.999999') as "stddev_samp/stddev_pop"
from a;
```

Because of the pseudorandom nature of `normal_rand()`, the values produced will change from run to run. Here are some typical values:

```
  avg   | stddev_samp | stddev_pop  | stddev_samp/stddev_pop 
--------+-------------+-------------+------------------------
  0.138 |   49.880052 |   49.879802 |   1.000005
```



