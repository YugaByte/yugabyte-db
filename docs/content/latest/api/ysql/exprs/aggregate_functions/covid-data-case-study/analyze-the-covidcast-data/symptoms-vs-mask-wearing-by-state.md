---
title: Select the data for COVID-like symptoms vs mask-wearing by state scatter plot for 21-Oct-2020
linkTitle: Data for scatter-plot for 21-Oct-2020
headerTitle: Select the data for COVID-like symptoms vs mask-wearing by state scatter plot for 21-Oct-2020
description: Select the data for COVID-like symptoms vs mask-wearing by state scatter plot for 21-Oct-2020
menu:
  latest:
    identifier: symptoms-vs-mask-wearing-by-state
    parent: analyze-the-covidcast-data
    weight: 20
isTocNested: true
showAsideToc: true
---

This query:

```plpgsql
select
  round(mask_wearing_pct)  as "% wearing mask",
  round(symptoms_pct)      as "% with symptoms",
  state
from covidcast_fb_survey_results_v
where survey_date = to_date('2020-10-21', 'yyyy-mm-dd')
order by 1;
```

selects out the data for 21-Oct-2020 so that they can be used to draw a scatter-plot. This is the result:

```
 % wearing mask | % with symptoms | state 
----------------+-----------------+-------
             66 |              34 | wy
             71 |              49 | sd
             73 |              46 | nd
             75 |              37 | id
             79 |              32 | ok
             80 |              31 | tn
             80 |              33 | ia
             80 |              29 | ms
             81 |              33 | mo
             81 |              41 | mt
             81 |              31 | ks
             81 |              24 | la
             82 |              36 | ne
             82 |              28 | al
             82 |              23 | ga
             83 |              29 | ak
             84 |              23 | sc
             85 |              33 | ut
             85 |              19 | fl
             86 |              31 | in
             86 |              30 | wv
             86 |              31 | ar
             86 |              23 | oh
             87 |              25 | tx
             87 |              17 | az
             87 |              29 | ky
             88 |              36 | wi
             88 |              20 | nc
             88 |              17 | pa
             88 |              21 | co
             88 |              22 | mi
             89 |              13 | me
             89 |              13 | nh
             89 |              26 | mn
             89 |              24 | il
             89 |              20 | nv
             90 |              21 | nm
             90 |              16 | or
             90 |              19 | va
             91 |              13 | ca
             92 |              16 | ri
             92 |              11 | vt
             92 |              16 | wa
             92 |              13 | nj
             93 |              15 | ct
             93 |              13 | ny
             93 |              15 | de
             94 |              16 | md
             94 |              12 | hi
             95 |              12 | ma
             96 |              12 | dc
```

The overall trend is clear: as mask-wearing percent increases, the incidence of COVID-like symptoms decreases.

You can re-run the query for several different arbitrarily selected dates. Or you can calculate the averages per state over the whole observation period like this:

```plpgsql
select
  round(avg(mask_wearing_pct))  as "% wearing mask",
  round(avg(symptoms_pct))      as "% with symptoms",
  state
from covidcast_fb_survey_results_v
group by state
order by 1;
```
The overall trend is always the same.
