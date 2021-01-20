---
title: Ingest the COVIDcast data into YugabyteDB
linkTitle: Ingest the COVIDcast data
headerTitle: Ingesting, checking, and combining the COVIDcast data
description: Ingest the COVIDcast data, check it for format compliance, and to combine it all into the single "covidcast_fb_survey_results" table
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: ingest-the-covidcast-data
    parent: covid-data-case-study
    weight: 20
isTocNested: true
showAsideToc: true
---


##  Ingest the .csv files, check the assumptions, and combine the interesting values into a single table

Here are the steps:

- [Manually inspect the .csv files](./inspect-the-csv-files).

- [Copy the data from each `.csv` file  "as is" into a dedicated staging table, with effective primary key _("state, survey_date)"_](./stage-the-csv-files). (The qualifier "effective" recognizes the fact that, as yet, these columns will have different names that reflect how they're named in the `.csv` files.)

- [Check that the values from the `.csv` files do indeed conform to the stated rules](./check-data-conforms-to-the-rules).

- [Project the columns of interest from the staging tables and join these into a single table](./join-the-staged-data/), with primary key _("state, survey_date)"_ for analysis.

All of these steps are implemented by the [`ingest-the-data.sql`](./ingest-scripts/ingest-the-data-sql/) script. It's designed so that you can run, and re-run, it time and again. It will always finish silently (provided that you say `set client_min_messages = warning;`) Each time you run it. It calls various other scripts. You will download these, along with [`ingest-the-data.sql`](./ingest-scripts/ingest-the-data-sql/), as you step through the sections in the order that the left-hand navigation menu presents.



