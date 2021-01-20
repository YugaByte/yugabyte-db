---
title: Avg symptoms vs avg mask-wearing by state
linkTitle: Scatter-plot for 21-Oct-2020
headerTitle: Average COVID-like symptoms vs average mask-wearing by state scatter plot for 21-Oct-2020
description: Average COVID-like symptoms vs average mask-wearing by state scatter plot for 21-Oct-2020
menu:
  latest:
    identifier: scatter-plot-for-21-Oct-2020
    parent: analyze-the-covidcast-data
    weight: 30
isTocNested: true
showAsideToc: true
---

## Scatter-plot for 21-Oct-2020

The dots on this plot represent _"mask_wearing_pct"_ on the x-axis with _"symptoms_pct"_ on the y-axis from this query: 

```plpgsql
select
  round(mask_wearing_pct)  as "% wearing mask",
  round(symptoms_pct)      as "% with symptoms",
  state
from covidcast_fb_survey_results_v
where survey_date = to_date('2020-10-21', 'yyyy-mm-dd')
order by 1;
```

The plot would be too cluttered if each of the _51_ points were labeled with its two-letter state abbreviation.

The plot was created simply by pasting a comma-separated list of _"mask_wearing_pct"_-_"symptoms_pct"_ pairs into a spreadsheet and by using the app's built-in functionality to create a scatter plot from such pairs values. The values were produced with this query:

```plpgsql
select
  round(mask_wearing_pct)::text||','||round(symptoms_pct)::text
from covidcast_fb_survey_results_v
where survey_date = to_date('2020-10-21', 'yyyy-mm-dd')
order by 1;
```

Then the plot was printed and the line was drawn in by hand using the slope and y-axis intercept from this query:

```plpgsql
with a as (
  select
    max(survey_date)                               as survey_date,
    regr_slope    (symptoms_pct, mask_wearing_pct) as s,
    regr_intercept(symptoms_pct, mask_wearing_pct) as i
  from covidcast_fb_survey_results_v
  where survey_date = to_date('2020-10-21', 'yyyy-mm-dd'))
select
  to_char(survey_date,      'mm/dd')  as survey_date,
  to_char(s,  '90.9')                 as s,
  to_char(i,  '990.9')                as i
from a;
```

This is the result:

```
 survey_date |   s   |   i    
-------------+-------+--------
 10/21       |  -1.2 |  131.4
```

And here is the plot:

![Scatter-plot for 2020-10-21](/images/api/ysql/exprs/aggregate_functions/covid-data-case-study/scatter-plot-2020-10-21.jpg)

## Scatter-plot for synthetic data

For comparison, the same technique was used to create a scatter-plot and to draw in the best-fit straight line using synthetic data by running the procedure _"populate_t()"_ described in the section [Create the test table](../../../function-syntax-semantics/linear-regression/#create-the-test-table) within the section that introduces the built-in aggregate functions for linear regression analysis.

```
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
  ...
```

This code, and the remaining code below, needed to make the scatter-plot for synthetic data is included in [`synthetic-data.sql`](../analysis-scripts/synthetic-data-sql/).

It uses the function `normal_rand()`, brought by the [tablefunc](../../../../../extensions/#tablefunc) extension to add pseudorandomly generated normally distributed noise the y-axis values produced by the _"y = m*x + c"_ formula for the straight line.

It was then invoked like this:

```plpgsql
call populate_t(
  no_of_rows  => 100,
  mean        => 0.0,
  stddev      => 5.0,
  slope       => -1.2,
  intercept   => 131.4);
```
using the values for slope and intercept from the regression analysis of the COVIDcast data for 21-Oct-2020 and by choosing a value for the _"stddev"_ actual argument arbitrarily.

The comma-separated pairs for the spreadsheet were produced by this query:

```plpgsql
select
  round(x)::text||','||round(y + delta)::text
from t
where
  x > 60        and
  x < 95        and
  x is not null and
  y is not null
order by x;
```

And the values for the slope and y-axis intercept were produced by this query:

```plpgsql
with a as (
  select
    regr_r2       ((y + delta), x) as r2,
    regr_slope    ((y + delta), x) as s,
    regr_intercept((y + delta), x) as i
  from t)
select
  to_char(r2,  '0.99')                as r2,
  to_char(s,  '90.9')                 as s,
  to_char(i,  '990.9')                as i
from a;
```

This is the result:

```
  r2   |   s   |   i    
-------+-------+--------
  0.98 |  -1.2 |  130.8
```

The emergent values for the slope and intercept are very close to the values (_-1.2_ and _131.4_) that were used for the invocation of _"populate_t()"_.

Here is the resulting plot

![Scatter-plot for synthetic data](/images/api/ysql/exprs/aggregate_functions/covid-data-case-study/scatter-plot-synthetic-data.jpg):

**Note:** The `normal_rand()` function produces a different set of pseudorandomly distributed values each time that  [`synthetic-data.sql`](../analysis-scripts/synthetic-data-sql/) is run. But the overall shape of the scatter-plot will remain the same.

