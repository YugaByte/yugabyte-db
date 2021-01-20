---
title: Analyze the COVIDcast data
linkTitle: Analyze the COVIDcast data
headerTitle: Using the YSQL linear regression analysis functions on the COVIDcast data—introduction
description: Using regr_r2(), regr_slope(), regr_intercept() on the COVIDcast data—introduction
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: analyze-the-covidcast-data
    parent: covid-data-case-study
    weight: 30
isTocNested: true
showAsideToc: true
---

## Introduction

Try this query:

```plpgsql
select max(symptoms_pct) from covidcast_fb_survey_results;
```

The result is about _2.7%_. This indicates that the signal "_symptoms_pct"_ (characterized on the [COVIDcast download page](../ingest-the-covidcast-data/inspect-the-csv-files/) by "Percentage of people with COVID-like symptoms, based on surveys of Facebook users") has little power of discrimination.

Now try this:

```plpgsql
select
  (select min(cmnty_symptoms_pct) as "Min" from covidcast_fb_survey_results),
  (select max(cmnty_symptoms_pct) as "Max" from covidcast_fb_survey_results);
```

The results here are about _7%_ for the minimum and about _55%_ for the maximum. This indicates that the signal "_cmnty_symptoms_pct"_ (characterized on the [COVIDcast download page](../ingest-the-covidcast-data/inspect-the-csv-files/) by "Percentage of people who know someone in their local community with COVID-like symptoms, based on surveys of Facebook users") will have a reasonable power of discrimination.

None of the YSQL built-in aggregate functions can take account of the _"stderr"_ or _"sample_size"_ values that were carried forward into the final _"covidcast_fb_survey_results"_ table. But you might like to try some _ad hoc_ queries to get an idea of the variability and reliability of the data.

For example, this:

```plpgsql
select avg(cmnty_symptoms_stderr) from covidcast_fb_survey_results;
```

gives a result of about _0.8_ for the percentage values in the range _7%_ through _55%_. This suggests that the seven day moving averages are reasonably reliable.

And this:

```plpgsql
select
  (select min(cmnty_symptoms_sample_size) as "Min" from covidcast_fb_survey_results),
  (select max(cmnty_symptoms_sample_size) as "Max" from covidcast_fb_survey_results);
```

results in about _325_ for the minimum and about _24.6_ thousand for the maximum. This is a rather troublesomely wide range. The result of this query:

```plpgsql
select
  round(avg(cmnty_symptoms_sample_size)) as "Avg",
  state
from covidcast_fb_survey_results
group by state
order by 1;
```

suggests that the sample size is probably correlated with the state's population. For example, the two biggest sample size values are from California and Texas. and the two smallest are from DC and Wyoming. It would be straightforward to find a list of recent values for state populations from the Internet and to join these, using state, into a table together with the average sample sizes from the query above. You could then use the [`regr_r2()`](../../function-syntax-semantics/linear-regression/regr/#regr-r2) function to see how well-correlated the size of a state's response to the COVIDcast Facebook survey is to its population. This is left as an exercise for the reader.

Create a view to focus your attention on the values that the analysis presented in the remainder of this section uses:

```plpgsql
create or replace view covidcast_fb_survey_results_v as
select
  survey_date,
  state,
  mask_wearing_pct,
  cmnty_symptoms_pct as symptoms_pct
from covidcast_fb_survey_results;
```

This is included in the [`analysis-queries.sql`](./analysis-scripts/analysis-queries-sql/) script that also implements all of the queries that the analysis presented in the remainder of this section uses.

If you want to see how the results come out when you use the _"symptoms_pct"_ column instead of the _"cmnty_symptoms_pct"_ column, just redefine the view, thus:

```plpgsql
create or replace view covidcast_fb_survey_results_v as
select
  survey_date,
  state,
  mask_wearing_pct,
  symptoms_pct as symptoms_pct
from covidcast_fb_survey_results;
```

## How the rest of this analysis section is organized

- The section [Daily values for regr_r2(), regr_slope(), regr_intercept() for symptoms vs mask-wearing](./daily-regression-analysis/) describes the actual linear regression analysis code.

- The section [Select the data for COVID-like symptoms vs mask-wearing by state scatter plot](./symptoms-vs-mask-wearing-by-state/) shows the SQL that lists out the _51_ individual _"(symptoms_pct, mask_wearing_pct)"_ tuples for the day that was arbitrarily chosen for drawing a scatter-plot on top of which the outcome of the regression analysis for that day is drawn.




