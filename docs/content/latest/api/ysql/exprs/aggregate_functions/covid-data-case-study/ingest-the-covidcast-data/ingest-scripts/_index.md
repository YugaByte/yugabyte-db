---
title: SQL scripts for ingesting the COVIDcast data
linkTitle: SQL scripts
headerTitle: SQL scripts for ingesting the COVIDcast data
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: ingest-scripts
    parent: ingest-the-covidcast-data
    weight: 100
isTocNested: true
showAsideToc: true
---

Here are the `.sql` scripts that jointly implement the whole ingestion flow: creating staging tables; copying in the data from the `.csv` files; checking that the data in the staging tables conforms to the expected rules; and projecting the relevant columns and joining them into a single _"covidcast_fb_survey_results"_ table.

- [`ingest-the-data.sql`](./ingest-the-data-sql) is the master script for this purpose. It contains some explicit SQL statements and it invokes other files that, for example, create procedures that the master script calls. Save it to the _"covid-data-case-study"_ directory that you created for this case study. You can't run it yet because the files that it needs aren't yet in place. You will step through each manually and then save it. When you've done this once, and saved all the files, you will then be able to run, and re-run, the whole ingestion process with a single "button press". This will be useful if you decide, later, to download more recent COVIDcast data.

  Here are the scripts that the master script relies on:

- [`cr-cr-staging-tables.sql`](./cr-cr-staging-tables-sql)

- [`cr-cr-copy-from-csv-scripts.sql`](./cr-cr-copy-from-csv-scripts-sql)

- [`cr-assert-assumptions-ok.sql`](./cr-assert-assumptions-ok-sql)

- [`cr-xform-to-covidcast-fb-survey-results.sql`](./cr-xform-to-joined-table-sql)
