---
title: analysis-queries.sql
linkTitle: analysis-queries.sql
headerTitle: SQL script to perform linear regression analysis on the COVIDcast data
description: SQL script to perform linear regression analysis using regr_r2(), regr_slope(), regr_intercept() on the COVIDcast data
menu:
  latest:
    identifier: analysis-queries-sql
    parent: analysis-scripts
    weight: 10
isTocNested: true
showAsideToc: true
---

**Save this script as "analysis-queries.sql"**

```plpgsql
create or replace view covidcast_fb_survey_results_v as
select
  survey_date,
  state,
  mask_wearing_pct,
  cmnty_symptoms_pct as symptoms_pct
from covidcast_fb_survey_results;

\o analysis-results/analysis-queries.txt
\t on
select 'Symptoms by state for survey date = 2020-10-21.';
\t off
select
  round(mask_wearing_pct)  as "% wearing mask",
  round(symptoms_pct)      as "% with symptoms",
  state
from covidcast_fb_survey_results_v
where survey_date = to_date('2020-10-21', 'yyyy-mm-dd')
order by 1;

\t on
select 'Symptoms by state, overall average.';
\t off
select
  round(avg(mask_wearing_pct))  as "% wearing mask",
  round(avg(symptoms_pct))      as "% with symptoms",
  state
from covidcast_fb_survey_results_v
group by state
order by 1;

\t on
select 'Daily regression analysis report.';
\t off
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

\t on
select 'Regression analysis report for survey date = 2020-10-21.';
\t off
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

with a as (
  select regr_r2 (symptoms_pct, mask_wearing_pct) as r2
  from covidcast_fb_survey_results_v
  group by survey_date)
select
  to_char(avg(r2), '0.99') as "avg(R-squared)"
from a;

\o

\o analysis-results/2020-10-21-mask-symptoms.csv
\t on
select
  round(mask_wearing_pct)::text||','||round(symptoms_pct)::text
from covidcast_fb_survey_results_v
where survey_date = to_date('2020-10-21', 'yyyy-mm-dd')
order by 1;
\t off
\o
```
