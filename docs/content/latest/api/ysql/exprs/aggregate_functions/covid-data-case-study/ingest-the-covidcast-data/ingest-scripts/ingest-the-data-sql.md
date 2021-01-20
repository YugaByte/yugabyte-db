---
title: ingest-the-data-master-script SQL script
linkTitle: ingest-the-data.sql
headerTitle: ingest-the-data.sql
description: One of a set of SQL scripts for analyzing the correlation between COVID-like symptoms and mask-wearing using data from Carnegie Mellon's COVIDcast
menu:
  latest:
    identifier: ingest-the-data-sql
    parent: ingest-scripts
    weight: 100
isTocNested: true
showAsideToc: true
---

**Save this script as "ingest-the-data.sql"**

```plpgsql
-- Set up the infrastructure and create the tables.

drop table if exists covidcast_names cascade;
create table covidcast_names(csv_file text primary key, staging_table text not null, signal text not null);

-- Each of these files contains 50 days of observations.
insert into covidcast_names(csv_file, staging_table, signal) values
  ('csv-files/covidcast-fb-survey-smoothed_'|| 'wearing_mask' ||'-2020-09-13-to-2020-11-01.csv', 'mask_wearers',   'smoothed_wearing_mask'),
  ('csv-files/covidcast-fb-survey-smoothed_'|| 'cli'          ||'-2020-09-13-to-2020-11-01.csv', 'symptoms',       'smoothed_cli'),
  ('csv-files/covidcast-fb-survey-smoothed_'|| 'hh_cmnty_cli' ||'-2020-09-13-to-2020-11-01.csv', 'cmnty_symptoms', 'smoothed_hh_cmnty_cli');

create unique index covidcast_names_staging_table_unq on covidcast_names(staging_table);
create unique index covidcast_names_signal_unq on covidcast_names(signal);

\i cr-cr-staging-tables.sql
\i cr-cr-copy-from-csv-scripts.sql

call cr_staging_tables();

--------------------------------------------------------------------------------
-- Import the CSV files into the staging tables;
\t on

\o copy_from_csv.sql
select cr_copy_from_scripts(1);
\o
\i copy_from_csv.sql

\o copy_from_csv.sql
select cr_copy_from_scripts(2);
\o
\i copy_from_csv.sql

\o copy_from_csv.sql
select cr_copy_from_scripts(3);
\o
\i copy_from_csv.sql

\t off
--------------------------------------------------------------------------------
-- Check that the imported data are consistent with what was assumed about its
-- format and content. If the checks pass, then merge it into the single
-- "covidcast_fb_survey_results" table.

\i cr-assert-assumptions-ok.sql
\i cr-xform-to-covidcast-fb-survey-results.sql

do $body$
begin
  -- If "assert_assumptions_ok()" aborts with an assert failure,
  -- then "cr_covidcast_fb_survey_results()" will not be called.
  call assert_assumptions_ok(
    start_survey_date => to_date('2020-09-13', 'yyyy-mm-dd'),
    end_survey_date   => to_date('2020-11-01', 'yyyy-mm-dd'));
  call xform_to_covidcast_fb_survey_results();
end;
$body$;
```
