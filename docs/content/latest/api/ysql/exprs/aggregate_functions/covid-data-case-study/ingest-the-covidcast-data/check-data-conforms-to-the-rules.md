---
title: Check the ingested COVIDcast data conforms to the rules
linkTitle: Check staged data conforms to the rules
headerTitle: Check that the values from the .csv files do indeed conform to the stated rules
menu:
  latest:
    identifier: check-data-conforms-to-the-rules
    parent: ingest-the-covidcast-data
    weight: 30
isTocNested: true
showAsideToc: true
---

The rules to which the values copied in from the `.csv` files must conform are described in the section [Inspect the COVIDcast .csv files](../inspect-the-csv-files). The form of each rule is the same for each of the three files. But sometimes, specific values that the rules govern are specific to the file. Each of the rules is tested, using the `assert` construct, in a single PL/pgSQL stored procedure, created by the script [`cr-assert-assumptions-ok.sql`](../ingest-scripts/cr-assert-assumptions-ok-sql). The tests share these common declarations:

```plpgsql
-- Each survey date (i.e. "time_value") has exactly the same number of states (i.e. "geo_value").
-- Each state has the same number, of survey date values.

expected_states constant text[] not null := array[
  'ak', 'al', 'ar', 'az', 'ca', 'co', 'ct', 'dc', 'de', 'fl', 'ga',
  'hi', 'ia', 'id', 'il', 'in', 'ks', 'ky', 'la', 'ma', 'md',
  'me', 'mi', 'mn', 'mo', 'ms', 'mt', 'nc', 'nd', 'ne', 'nh',
  'nj', 'nm', 'nv', 'ny', 'oh', 'ok', 'or', 'pa', 'ri', 'sc',
  'sd', 'tn', 'tx', 'ut', 'va', 'vt', 'wa', 'wi', 'wv', 'wy'
];

expected_state_count constant int := cardinality(expected_states);

actual_states_qry constant text not null :=
  'select array_agg(distinct geo_value order by geo_value) from ?';

actual_states text[] not null := '{}';

expected_dates date[] not null := array[start_survey_date];

actual_dates_qry constant text not null :=
  'select array_agg(distinct time_value order by time_value) from ?';

actual_dates date[] not null := '{}';

expected_date_count int not null := 0;

names constant covidcast_names[] not null := (
  select array_agg((csv_file, staging_table, signal)::covidcast_names) from covidcast_names);

expected_total_count int not null := 0;

r covidcast_names not null := ('', '', '');
d  date     not null := start_survey_date;
t  text     not null := '';
n  int      not null := 0;
b  boolean  not null := false;
```

## Tests applied in turn to each staging table

The first several tests are applied, in turn, to each staging table. It is therefore convenient to implement them using dynamic SQL in a loop that iterates over the elements in the _"names"_ array, thus:

```plpgsql
foreach r in array names loop
  ...
end loop;
```

### The "signal" column

Check that every value in the ["signal"](../inspect-the-csv-files/#signal) column is the same and that it is equal to the value that is specific to the source `csv` file's name.

```
execute replace('select distinct signal from ?', '?', r.staging_table) into t;
assert t = r.signal, 'signal from '||r.staging_table||' <> "'||r.signal||'"';
```

### The "geo_type" column

Check that every value in the ["geo_type"](../inspect-the-csv-files/#data-source-and-geo-type) column is the same and that it is equal to _'state'_:

```
execute 'select distinct geo_type from '||r.staging_table into t;
assert t = 'state', 'geo_type from '||r.staging_table||' <> "state"';
```

### The "data_source" column

Check that every value in the ["data_source"](../inspect-the-csv-files/#data-source-and-geo-type) column is the same and that it is equal to _'fb-survey'_:

```
execute 'select distinct data_source from '||r.staging_table into t;
assert t = 'fb-survey', 'data_source from '||r.staging_table||' <> "fb-survey"';
```

### The "direction" column

Check that every value in the ["direction"](../inspect-the-csv-files/#direction) columns `NULL`:

```
execute $$select distinct coalesce(direction, '<null>') from $$||r.staging_table into t;
assert t = '<null>', 'direction from '||r.staging_table||' <> "<null>"';
```

### Expected total count(*)

Check that the total number of rows is equal to _"(number of distinct states)*(number of distinct survey dates)"_:

```
execute 'select count(*) from '||r.staging_table into n;
assert n = expected_total_count, 'count from '||r.staging_table||' <> expected_total_count';
```

### The "geo_value" column

Check that there are _51_ distinct values of ["geo_value"](../inspect-the-csv-files/#code-and-geo-value), corresponding to the _51_ actual states (including DC):

```
execute replace(actual_states_qry, '?', r.staging_table) into actual_states;
assert actual_states = expected_states, 'actual_states <> expected_states';

execute 'select count(distinct geo_value) from '||r.staging_table into n;
assert n = expected_state_count, 'distinct state count per survey date from '||r.staging_table||' <> expected_state_count';
```

### The "time_value" column

Check that there are _50_ distinct values of ["time_value"](../inspect-the-csv-files/#time-value), corresponding to the values in the dense sequence that the download range, _"2020-09-13 - 2020-11-01"_, specified:

```
execute replace(actual_dates_qry, '?', r.staging_table) into actual_dates;
assert actual_dates = expected_dates, 'actual_dates <> expected_dates';

execute 'select count(distinct time_value) from '||r.staging_table into n;
assert n = expected_date_count, 'distinct survey date count per state from '||r.staging_table||' <> expected_date_count';
```

### Check that each survey date has the same number of states

Because this is a `SELECT DISTINCT ... INTO` query, it will cause an error if the number of states per survey date isn't the same for each date.

```
execute '
  with r as (
    select time_value, count(time_value) as n from '||r.staging_table||'
    group by time_value)
  select distinct n from r' into n;
assert n = expected_state_count, 'distinct state count from '||r.staging_table||' <> expected_state_count';
```

### Check that each state has the same number of survey dates

Because this is a `SELECT DISTINCT ... INTO` query, it will cause an error if the number of survey dates per state isn't the same for each date.

```
execute '
  with r as (
    select geo_value, count(geo_value) as n from '||r.staging_table||'
    group by geo_value)
  select distinct n from r' into n;
assert n = expected_survey_date_count, 'distinct state count from '||r.staging_table||' <> expected_survey_date_count';
```

### The "value" column

Check that the ["value"](../inspect-the-csv-files/#value) column is a number in the range _0..100_.

```
execute '
  select
    max(value) between 0 and 100 and
    min(value) between 0 and 100
  from '||r.staging_table into b;
assert b, 'max(value), min(value) from '||r.staging_table||' both < 100 FALSE';
```

## Tests applied in a single check across all three staging tables

These tests need to check that sets of values in each of the three staging tables are the same. They therefore use the `UNION ALL` of `EXCEPT` queries pattern to perform the test in a single `SELECT` that is expected to produce no rows.

### Check the one-to-one correspondence between "code" and "geo_code"

Each state has a unique numeric _"code"_. The following `SELECT` confirms that this is the case. The _"code"_ column can therefore be safely left out of the final _"covidcast_fb_survey_results"_ table.

```
declare
  chk_code_and_geo_values constant text := $$
  with
    a1 as (
      select to_char(code, '90')||' '||geo_value as v from ?1),
    v1 as (
      select v, count(v) as n from a1 group by v),
    a2 as (
      select to_char(code, '90')||' '||geo_value as v from ?2),
    v2 as (
      select v, count(v) as n from a2 group by v),
    a3 as (
      select to_char(code, '90')||' '||geo_value as v from ?3),
    v3 as (
      select v, count(v) as n from a3 group by v),

    v4 as (select v, n from v1 except select v, n from v2),
    v5 as (select v, n from v2 except select v, n from v1),
    v6 as (select v, n from v1 except select v, n from v3),
    v7 as (select v, n from v3 except select v, n from v1),

    r as (
      select v, n from v4
      union all
      select v, n from v5
      union all
      select v, n from v6
      union all
      select v, n from v6)

  select count(*) from r$$;
begin
  execute replace(replace(replace(chk_code_and_geo_values, 
  '?1', names[1].staging_table),
  '?2', names[2].staging_table),
  '?3', names[3].staging_table
  ) into n;

  assert n = 0, '(code, geo_value) tuples from the three staging tables disagree';
end;
```

### Check that each staging table has the same set of (geo_value, time_value) primary key tuples

```
declare
  chk_putative_pks constant text := '
    with
      v1 as (
        select geo_value, time_value from ?1
        except
        select geo_value, time_value from ?2),

      v2 as (
        select geo_value, time_value from ?2
        except
        select geo_value, time_value from ?1),

      v3 as (
        select geo_value, time_value from ?1
        except
        select geo_value, time_value from ?3),

      v4 as (
        select geo_value, time_value from ?3
        except
        select geo_value, time_value from ?1),

      v5 as (
        select geo_value, time_value from v1
        union all
        select geo_value, time_value from v2
        union all
        select geo_value, time_value from v3
        union all
        select geo_value, time_value from v4)

    select count(*) from v5';
begin
  execute replace(replace(replace(chk_putative_pks,
      '?1', names[1].staging_table),
      '?2', names[2].staging_table),
      '?3', names[3].staging_table)
    into n;

  assert n = 0, 'pk values from ' ||
    replace(replace(replace('?1, ?2, ?3',
      '?1', names[1].staging_table),
      '?2', names[2].staging_table),
      '?3', names[3].staging_table) ||
    ' do not line up';
end;
```

## Create the assert_assumptions_ok() procedure and invoke it manually

Copy the [_"cr_assert_assumptions_ok()"_](../ingest-scripts/cr-assert-assumptions-ok-sql) code and paste it into `ysqlsh`. Then do this:

```plpgsql
call assert_assumptions_ok(
  start_survey_date => to_date('2020-09-13', 'yyyy-mm-dd'),
  end_survey_date   => to_date('2020-11-01', 'yyyy-mm-dd'));
```

It will finish silently. You can provoke and `assert` failure by invoking it with, say:

```
end_survey_date  => to_date('2020-11-02', 'yyyy-mm-dd')
```
The error is reported thus:

```
count from cmnty_symptoms <> expected_total_count
```

The invocation is included in [`ingest-the-data.sql`](../ingest-scripts/ingest-the-data-sql).