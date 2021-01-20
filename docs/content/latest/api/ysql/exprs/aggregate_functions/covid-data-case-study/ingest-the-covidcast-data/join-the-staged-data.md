---
title: Join the staged COVIDcast data into a single table
linkTitle: Join the staged data into a single table
headerTitle: Join the staged COVIDcast data into the "covidcast_fb_survey_results" table
menu:
  latest:
    identifier: join-the-staged-data
    parent: ingest-the-covidcast-data
    weight: 40
isTocNested: true
showAsideToc: true
---

The section [Inspect the COVIDcast .csv files](../inspect-the-csv-files/) concluded by showing that the three staging tables, one for each `.csv` file, have the same primary key whose columns have the meaning _"(survey_date, state)"_. This means that the data in all of the tables can be joined using this primary key into a single table. It showed, further, that the three "payload" columns in each of the staging tables, which there have the same names—_"value"_, _"stderr"_, and _"sample_size"_—can be carried over by the join into columns whose names reflect their origin and therefore their ultimate meaning, thus:

```
VALUE        # Will go to MASK_WEARING_PCT,          SYMPTOMS_PCT,          or CMNTY_SYMPTOMS_PCT.
STDERR       # Will go to MASK_WEARING_STDERR,       SYMPTOMS_STDERR,       or CMNTY_SYMPTOMS_STDERR.
SAMPLE_SIZE  # Will go to MASK_WEARING_SAMPLE_SIZE,  SYMPTOMS_SAMPLE_SIZE,  or CMNTY_SYMPTOMS_SAMPLE_SIZE.
```

Here is the statement that creates the table:

```plpgsql
  drop table if exists covidcast_fb_survey_results cascade;

  create table covidcast_fb_survey_results(
    survey_date                 date     not null,
    state                       text     not null,
    mask_wearing_pct            numeric  not null,
    mask_wearing_stderr         numeric  not null,
    mask_wearing_sample_size    int      not null,
    symptoms_pct                numeric  not null,
    symptoms_stderr             numeric  not null,
    symptoms_sample_size        int      not null,
    cmnty_symptoms_pct          numeric  not null,
    cmnty_symptoms_stderr       numeric  not null,
    cmnty_symptoms_sample_size  int      not null,

    constraint covidcast_fb_survey_results_pk primary key (state, survey_date),

    constraint covidcast_fb_survey_results_chk_mask_wearing_pct    check(mask_wearing_pct   between 0 and 100),
    constraint covidcast_fb_survey_results_chk_symptoms_pct        check(symptoms_pct       between 0 and 100),
    constraint covidcast_fb_survey_results_chk_cmnty_symptoms_pct  check(cmnty_symptoms_pct between 0 and 100),

    constraint covidcast_fb_survey_results_chk_mask_wearing_stderr    check(mask_wearing_stderr   > 0),
    constraint covidcast_fb_survey_results_chk_symptoms_stderr        check(symptoms_stderr       > 0),
    constraint covidcast_fb_survey_results_chk_cmnty_symptoms_stderr  check(cmnty_symptoms_stderr > 0),

    constraint covidcast_fb_survey_results_chk_mask_wearing_sample_size    check(mask_wearing_sample_size   > 0),
    constraint covidcast_fb_survey_results_chk_symptoms_sample_size        check(symptoms_sample_size       > 0),
    constraint covidcast_fb_survey_results_chk_cmnty_symptoms_sample_size  check(cmnty_symptoms_sample_size > 0)
  );
```

The constraints honor the same principle that the use of the [_"assert_assumptions_ok()"_](../ingest-scripts/cr-assert-assumptions-ok-sql) procedure does:

- to check, explicitly, that all of the documented rules about the source data hold so that the results of applying the linear regression analysis can be trusted.

The procedure ["_xform_to_covidcast_fb_survey_results()"_](../ingest-scripts/cr-xform-to-joined-table-sql) creates and populates the _"covidcast_fb_survey_results"_ table. It uses dynamic SQL so that it can use the names for the staging tables that are defined in the _"covidcast_names"_ table. This is the logic:

```
declare
  mask_wearers_name    text not null := (select staging_table from covidcast_names where staging_table = 'mask_wearers');
  symptoms_name        text not null := (select staging_table from covidcast_names where staging_table = 'symptoms');
  cmnty_symptoms_name  text not null := (select staging_table from covidcast_names where staging_table = 'cmnty_symptoms');

  stmt text not null := '
    insert into covidcast_fb_survey_results(
      survey_date, state,
      mask_wearing_pct,    mask_wearing_stderr,    mask_wearing_sample_size,
      symptoms_pct,        symptoms_stderr,        symptoms_sample_size,
      cmnty_symptoms_pct,  cmnty_symptoms_stderr,  cmnty_symptoms_sample_size)
    select
      time_value, geo_value,
      m.value, m.stderr, round(m.sample_size),
      s.value, s.stderr, round(s.sample_size),
      c.value, c.stderr, round(c.sample_size)
    from
      ?1 as m
      inner join ?2 as s using (time_value, geo_value)
      inner join ?3 as c using (time_value, geo_value)';
begin
    execute replace(replace(replace(stmt,
    '?1', mask_wearers_name),
    '?2', symptoms_name),
    '?3', cmnty_symptoms_name);
  ...
end;
```

Invoke the join into the final table manually, thus:

```plpgsql
call xform_to_covidcast_fb_survey_results();
```

The [`ingest-the-data.sql`](../ingest-scripts/ingest-the-data-sql) script creates the procedure _"xform_to_covidcast_fb_survey_results()"_ and then invokes it like this:

```plpgsql
do $body$
begin
  call assert_assumptions_ok(
    start_survey_date => to_date('2020-09-13', 'yyyy-mm-dd'),
    end_survey_date   => to_date('2020-11-01', 'yyyy-mm-dd'));
  call xform_to_covidcast_fb_survey_results();
end;
$body$;
```

Notice that if [_"assert_assumptions_ok()"_](../ingest-scripts/cr-assert-assumptions-ok-sql) aborts with an assert failure, then _"cr_covidcast_fb_survey_results()"_ will not be called.


