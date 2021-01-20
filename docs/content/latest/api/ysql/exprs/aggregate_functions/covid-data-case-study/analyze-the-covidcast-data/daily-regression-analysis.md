---
title: Daily values for regr_r2(), regr_slope(), regr_intercept() for symptoms vs mask-wearing
linkTitle: symptoms vs mask-wearing by day
headerTitle: Daily values for regr_r2(), regr_slope(), regr_intercept() for symptoms vs mask-wearing
description: Reporting the daily values for regr_r2(), regr_slope(), regr_intercept() for symptoms vs mask-wearing
menu:
  latest:
    identifier: report-daily-regression-analysis
    parent: analyze-the-covidcast-data
    weight: 10
isTocNested: true
showAsideToc: true
---

This is the semantic essence of the query:

```plpgsql
select
                                                    survey_date,
  avg           (mask_wearing_pct)               as mask_wearing_pct,
  avg           (symptoms_pct)                   as symptoms_pct,
  regr_r2       (symptoms_pct, mask_wearing_pct) as r2,
  regr_slope    (symptoms_pct, mask_wearing_pct) as s,
  regr_intercept(symptoms_pct, mask_wearing_pct) as i
from covidcast_fb_survey_results_v
group by survey_date
order by survey_date;
```

It acts, in turn, on the data for all _51_ states for each day to show the daily regression analysis. You can understand the results for a particular day by picturing a scatter-plot for that day with mask-wearing (the putative _independent_ variable) along the x-axis and incidence of COVID-like symptoms (the putative _dependent_ variable) along the y-axis. The plot will have _51_ points, one for each state. The values returned by [`regr_slope()`](../../../function-syntax-semantics/linear-regression/regr/#regr-slope-regr-intercept) and [`regr_intercept()`](../../../function-syntax-semantics/linear-regression/regr/#regr-slope-regr-intercept) allow the line that minimizes the [residuals](https://statisticsbyjim.com/glossary/residuals/) through the points to be drawn. And the value returned by [`regr_r2()`](../../../function-syntax-semantics/linear-regression/regr/#regr-r2) is a measure of the noisiness of the data. It's usefulness is somewhat analogous to the variance of these residuals—except that the maximum possible value, _1.0_, indicates a perfect fit (i.e. all the residuals are zero) and successively smaller values indicate larger variance over the set of residuals. A more careful account is given in the section [Functions for linear regression analysis](../../../function-syntax-semantics/linear-regression/). Here's the rule:

- When [`regr_r2()`](../../../function-syntax-semantics/linear-regression/regr/#regr-r2) returns a value of _0.6_, it means that _60%_ of the relationship of the y-axis variable (the first actual in the function invocation) to the x-axis variable (the second actual in the function invocation) is explained by a simple _"y = m*x + c"_ linear dependence—and that the remaining _40%_ is unexplained. A value greater than about _60%_ is generally taken to indicate that the y-axis variable really does depend upon the x-axis variable.

Such a scatter-plot for a particular arbitrarily selected day in the middle of the observation period, with the fitted line drawn in, is shown in the section [Average COVID-like symptoms vs average mask-wearing by state scatter plot](../scatter-plot-for-2020-10-21/).

Here is the actual query that was used (see [SQL script to perform linear regression analysis on the COVIDcast data](../analysis-scripts/analysis-queries-sql/)). The values are formatted for readability by defining the basic the  query in a `WITH` clause and by applying the formatting functions in the query's main part.

```plpgsql
with a as (
  select
                                                      survey_date,
    avg           (mask_wearing_pct)               as mask_wearing_pct,
    avg           (symptoms_pct)                   as symptoms_pct,
    regr_r2       (symptoms_pct, mask_wearing_pct) as r2,
    regr_slope    (symptoms_pct, mask_wearing_pct) as s,
    regr_intercept(symptoms_pct, mask_wearing_pct) as i
  from covidcast_fb_survey_results_v
  group by survey_date)
select
  to_char(survey_date,      'mm/dd')  as survey_date,
  to_char(mask_wearing_pct,    '90')  as mask_wearing_pct,
  to_char(symptoms_pct,  '90')        as symptoms_pct,
  to_char(r2,  '0.99')                as r2,
  to_char(s,  '90.9')                 as s,
  to_char(i,  '990.9')                as i
from a
order by survey_date;
```

See the [`analysis-queries.sql`](./../analysis-scripts/analysis-queries-sql/) script. Here are the results:

```
 survey_date | mask_wearing_pct | symptoms_pct |  r2   |   s   |   i    
-------------+------------------+--------------+-------+-------+--------
 09/13       |  85              |  19          |  0.54 |  -0.6 |   71.0
 09/14       |  85              |  19          |  0.55 |  -0.6 |   71.9
 09/15       |  85              |  19          |  0.58 |  -0.7 |   74.8
 09/16       |  85              |  19          |  0.59 |  -0.7 |   77.0
 09/17       |  85              |  19          |  0.57 |  -0.7 |   76.1
 09/18       |  85              |  19          |  0.59 |  -0.7 |   79.0
 09/19       |  85              |  19          |  0.62 |  -0.7 |   81.9
 09/20       |  85              |  20          |  0.61 |  -0.7 |   81.5
 09/21       |  85              |  20          |  0.60 |  -0.7 |   81.2
 09/22       |  85              |  20          |  0.57 |  -0.7 |   79.0
 09/23       |  85              |  20          |  0.59 |  -0.7 |   79.1
 09/24       |  85              |  20          |  0.62 |  -0.7 |   83.1
 09/25       |  85              |  20          |  0.62 |  -0.8 |   84.4
 09/26       |  85              |  20          |  0.61 |  -0.8 |   85.6
 09/27       |  85              |  20          |  0.63 |  -0.8 |   90.4
 09/28       |  85              |  21          |  0.66 |  -0.8 |   91.8
 09/29       |  85              |  21          |  0.69 |  -0.9 |   95.5
 09/30       |  85              |  21          |  0.70 |  -0.9 |   99.9
 10/01       |  85              |  21          |  0.70 |  -0.9 |  100.9
 10/02       |  85              |  21          |  0.70 |  -0.9 |  101.4
 10/03       |  85              |  21          |  0.68 |  -0.9 |   99.2
 10/04       |  85              |  22          |  0.66 |  -0.9 |   97.2
 10/05       |  85              |  22          |  0.69 |  -0.9 |  102.3
 10/06       |  86              |  23          |  0.68 |  -0.9 |  103.4
 10/07       |  86              |  23          |  0.66 |  -0.9 |  103.4
 10/08       |  86              |  23          |  0.64 |  -0.9 |  103.9
 10/09       |  86              |  23          |  0.65 |  -1.0 |  106.2
 10/10       |  86              |  23          |  0.65 |  -1.0 |  109.1
 10/11       |  86              |  24          |  0.66 |  -1.0 |  111.5
 10/12       |  86              |  24          |  0.62 |  -1.0 |  108.7
 10/13       |  86              |  23          |  0.61 |  -1.0 |  105.0
 10/14       |  86              |  23          |  0.61 |  -1.0 |  105.4
 10/15       |  86              |  23          |  0.63 |  -1.0 |  109.1
 10/16       |  86              |  24          |  0.63 |  -1.0 |  112.0
 10/17       |  86              |  24          |  0.65 |  -1.1 |  114.8
 10/18       |  86              |  24          |  0.64 |  -1.1 |  115.6
 10/19       |  86              |  24          |  0.68 |  -1.1 |  121.7
 10/20       |  86              |  24          |  0.68 |  -1.2 |  127.5
 10/21       |  86              |  24          |  0.69 |  -1.2 |  131.4
 10/22       |  86              |  24          |  0.68 |  -1.3 |  133.2
 10/23       |  86              |  24          |  0.67 |  -1.2 |  130.9
 10/24       |  86              |  25          |  0.65 |  -1.2 |  130.4
 10/25       |  86              |  25          |  0.66 |  -1.3 |  135.2
 10/26       |  86              |  25          |  0.63 |  -1.3 |  133.7
 10/27       |  87              |  25          |  0.64 |  -1.3 |  136.8
 10/28       |  87              |  26          |  0.62 |  -1.3 |  137.4
 10/29       |  87              |  26          |  0.62 |  -1.3 |  139.1
 10/30       |  88              |  26          |  0.62 |  -1.3 |  143.7
 10/31       |  88              |  26          |  0.59 |  -1.4 |  145.2
 11/01       |  88              |  26          |  0.57 |  -1.3 |  141.2
```

The semantics of the average mask-wearing percentage and the average percentage of people who know someone in their local community with COVID-like symptoms are straightforward and are included as information that is interesting for revealing a general time-dependent trend. The trend is depressing:

- Mask-wearing edges up, in fact monotonically, over the observation period.
- But the incidence of COVID-like symptoms climbs too, and again monotonically.

The hope would be that increased mask-wearing would lead to reduced incidence of COVID-like symptoms. Indeed, this effect is substantiated, for each individual date, by the regression analysis results across the set of 51 states for that date. So there must be other factors at work that influence the long-term trend of the across-state averages over a period of days. But the data at hand cannot shed light on these.

This query (also included in the [`analysis-queries.sql`](./../analysis-scripts/analysis-queries-sql/) script) calculates the average of the daily regression analysis results:

```plpgsql
with a as (
  select regr_r2 (symptoms_pct, mask_wearing_pct) as r2,
  regr_slope    (symptoms_pct, mask_wearing_pct) as s,
  regr_intercept(symptoms_pct, mask_wearing_pct) as i
  from covidcast_fb_survey_results_v
  group by survey_date)
select
  to_char(avg(r2), '0.99') as "avg(R-squared)",
  to_char(avg(s), '0.99') as "avg(s)",
  to_char(avg(i), '990.99') as "avg(i)"
from a;
```

This is the result:

```
 avg(R-squared) | avg(s) | avg(i)  
----------------+--------+---------
  0.63          | -0.97  |  105.59
```

The outcome of the regression analysis, for any particular day, is best understood with the help of a picture: a so-called scatter-plot which shows the input data with the line that best fits the data superimposed. The following section, [Select the data for COVID-like symptoms vs mask-wearing by state scatter plot](../symptoms-vs-mask-wearing-by-state/), shows the query that gets the data. And the section that follows that, [Average COVID-like symptoms vs average mask-wearing by state scatter plot for 21-Oct-2020](../scatter-plot-for-2020-10-21/), shows the picture.
