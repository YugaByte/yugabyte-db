---
title: case study—using aggregate functions to demonstrate the "68–95–99.7" rule
linkTitle: case study—percentile_cont() and the "68–95–99.7" rule
headerTitle: Case study—using aggregate functions to demonstrate the "68–95–99.7" rule
description: Case study to show the use of percentile_cont() to illustrate the well-known "68–95–99.7" rule about a normal distribution. 
menu:
  latest:
    identifier: case-study-the-68–95–997-rule
    parent: aggregate-functions
    weight: 100
isTocNested: true
showAsideToc: true
---

This case study shows the use of aggregate functions to demonstrate the so-called "68–95–99.7 rule"—described in [this Wikipedia article](https://en.wikipedia.org/wiki/68–95–99.7_rule). This case-study  focuses on just one part of the rule:

- 68.27% of the values in a normal distribution lie within one standard deviation each side of the mean.

## Populate the test table

The demonstration uses the function `normal_rand()`, brought by the [tablefunc](../../../extensions/#tablefunc) extension, to populate the test table:

```plpgsql
drop table if exists t cascade;
create table t(v double precision primary key);

do $body$
declare
  no_of_rows constant int              := 1000000;
  mean       constant double precision := 0.0;
  stddev     constant double precision := 50.0;
begin
  insert into t(v)
  select normal_rand(no_of_rows, mean, stddev);
end;
$body$;
```
If you choose a value of one million for _"no_of_rows"_, then the two different estimates for the one sigma boundaries reliably produce results that typically differ from each other by less than about 0.1%. If you choose fewer rows, then the variability, and the typical results difference, will be bigger.

Because the demonstration (for convenience) uses a table with a single `double precision` column, _"v"_,  this must be the primary key. It's just possible that `normal_rand()` will create some duplicate values. However, this is so very rare that it was never seen while the script was repeated, many times, during the development of this code example. If `insert into t(v)` does fail because of this, just repeat the script by hand.

## Create, and execute, the  table function "the_6827_rule()"

The table function _"the_6827_rule()"_, below, uses the following approach:

- _Firstly_, it determines the one sigma boundaries ordinarily by using the [`avg()`](../function-syntax-semantics/avg-count-max-min-sum/#avg) and [`stddev_pop()`](../function-syntax-semantics/variance-stddev/#stddev-pop) aggregate functions.

- _Secondly_, it determines these boundaries by invoking the [`percentile_cont()`](../function-syntax-semantics/mode-percentile-disc-percentile-cont/#percentile-disc-percentile-cont) aggregate function with putative [`percent_rank()`](../../window_functions/function-syntax-semantics/percent-rank-cume-dist-ntile/#percent-rank) input values corresponding, respectively, to the fraction of the table's values that lie below _"mean - one standard deviation"_ and below _"mean + one standard deviation"_. 

```plpgsql
drop function if exists the_6827_rule() cascade;
create function the_6827_rule()
  returns table (t text)
  language plpgsql
as $body$
declare
  -- FIRST, determine the one sigma boundaries by using avg() and stddev_pop().
  measured_avg
    constant double precision := (
      select avg(v) from t);

  measured_sigma
    constant double precision := (
      select stddev_pop(v) from t);

  one_sigma_boundaries_from_avg_and_stddev
    constant double precision[] := array[
      measured_avg - measured_sigma,
      measured_avg + measured_sigma];

  -- SECOND, determine the one sigma boundaries by using percentile_cont().
  fraction_within_sigma_from_avg
    constant double precision :=
      0.6827;

  expected_fraction_below_avg_minus_sigma
    constant double precision :=
      (1.0 - fraction_within_sigma_from_avg)/2.0;

  expected_fraction_above_avg_plus_sigma
    constant double precision :=
      expected_fraction_below_avg_minus_sigma +
      fraction_within_sigma_from_avg;

  fractions constant double precision[] := array[
    expected_fraction_below_avg_minus_sigma,
    expected_fraction_above_avg_plus_sigma];

  one_sigma_boundaries_from_percentile_cont
    constant double precision[] := (
      select
        percentile_cont(fractions)
        within group (order by v)
    from t);

begin
  t := rpad(' ',                            13)||
        lpad('from avg() and stddev_pop()', 29, ' ')||
        lpad('from percentile_cont()',      24, ' ')||
        lpad('ratio',                       10, ' ');

  return next;
  t := lpad(' ', 13)||rpad('  ', 29, '-')||rpad('  ', 24, '-')||rpad('  ', 10, '-');
  return next;

  for j in 1..2 loop
    declare
      caption constant text not null :=
        case j
          when 1 then 'mean - sigma:'
          when 2 then 'mean + sigma:'
        end;

      num constant double precision not null :=
        one_sigma_boundaries_from_percentile_cont[j] -
        one_sigma_boundaries_from_avg_and_stddev[j];

      denom constant double precision not null :=
        (one_sigma_boundaries_from_percentile_cont[j] +
         one_sigma_boundaries_from_avg_and_stddev[j])/2.0;

      ratio constant double precision not null :=
        abs((100.0::double precision*num)/denom);
    begin
      t := rpad(caption, 13)||
           lpad(to_char(one_sigma_boundaries_from_avg_and_stddev[j],  '990.99999'), 29)||
           lpad(to_char(one_sigma_boundaries_from_percentile_cont[j], '990.99999'), 24)||
           lpad(to_char(ratio, '990.999'), 9)||'%';
      return next;
    end;
  end loop;
end;
$body$;
```

A table function is used because if `raise info` is used in a procedure or anonymous block, then the output cannot be spooled to a file. Invoke it, for example, like this:

```plpgsql
\t on
\o report.txt
select t from the_6827_rule();
\o
\t off
```



## Typical result

Here is a typical result (for one million rows):

```
                from avg() and stddev_pop()  from percentile_cont()     ratio
                ---------------------------  ----------------------  --------
 mean - sigma:                    -50.00899               -49.99646    0.025%
 mean + sigma:                     49.97396                50.00483    0.062%
```
