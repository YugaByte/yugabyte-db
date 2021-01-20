---
title: SQL scripts for analyzing the COVIDcast data
linkTitle: SQL scripts
headerTitle: SQL scripts for performing linear regression analysis on the COVIDcast data
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: analysis-scripts
    parent: analyze-the-covidcast-data
    weight: 100
isTocNested: true
showAsideToc: true
---

Here are the `.sql` scripts that jointly implement analysis:

- [`analysis-queries.sql`](./analysis-queries-sql) executes queries on the actual COVIDcast data.

- [`synthetic-data.sql`](./synthetic-data-sql) generates the data that are used to create the plot descibed in the section [Scatter-plot for synthetic data](../scatter-plot-for-2020-10-21/#scatter-plot-for-synthetic-data).
