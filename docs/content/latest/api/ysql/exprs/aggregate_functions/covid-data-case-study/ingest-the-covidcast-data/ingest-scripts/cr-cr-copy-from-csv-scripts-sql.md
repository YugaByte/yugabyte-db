---
title: Create function cr_copy_from_scripts()
linkTitle: Create cr_copy_from_scripts()
headerTitle: Create the function cr_copy_from_scripts()
description: Creates a function that writes the chosen one of three \COPY command invocations.
menu:
  latest:
    identifier: cr-cr-copy-from-csv-scripts-sql
    parent: ingest-scripts
    weight: 20
isTocNested: true
showAsideToc: true
---

This procedure makes use of the [`row_number()`](../../../../../../exprs/window_functions/function-syntax-semantics/row-number-rank-dense-rank/#row-number) Window function to enable a single invocation of the function to choose the first, the second, or the third `.csv` file with respect to the alphabetical order of their filenames.

**Save this script as "cr-cr-copy-from-csv-scripts.sql"**

```plpgsql
drop function if exists cr_copy_from_scripts(int) cascade;

create function cr_copy_from_scripts(which in int)
  returns text
  language plpgsql
as $body$
<<b>>declare
  copy_from_csv constant text :=
    $$\copy ?1 from '?2' with (format 'csv', header true);$$;

  csv_file       text not null := '';
  staging_table  text not null := '';
begin
  with a as (
    select
      row_number() over (order by s.csv_file) as r,
      s.csv_file,
      s.staging_table
    from covidcast_names as s)
  select a.csv_file, a.staging_table
  into b.csv_file, b.staging_table
  from a where a.r = which;

  return replace(replace(
    copy_from_csv,
    '?1',
    staging_table),
    '?2',
    csv_file);
end b;
$body$;
```
