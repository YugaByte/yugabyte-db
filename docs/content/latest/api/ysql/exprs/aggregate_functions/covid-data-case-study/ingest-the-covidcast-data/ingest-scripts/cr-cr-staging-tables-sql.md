---
title: Create procedure cr_staging_tables()
linkTitle: Create cr_staging_tables()
headerTitle: Create the procedure cr_staging_tables()
description: Creates a procedure that uses dynamic SQL to create three staging tables with the same structure.
menu:
  latest:
    identifier: cr-cr-staging-tables-sql
    parent: ingest-scripts
    weight: 10
isTocNested: true
showAsideToc: true
---

This procedure makes use of the [`array_agg()`](../../../../../../datatypes/type_array/functions-operators/array-agg-unnest/#array-agg) function to get all the values as a `text[]` array in a single PL/pgSQL-to-SQL round trip. And it uses the terse [`FOREACH`](../../../../../../datatypes/type_array/looping-through-arrays/) construct to iterate of the array's values.

**Save this script as "cr-cr-staging-tables.sql"**

```plpgsql
drop procedure if exists cr_staging_tables() cascade;

create procedure cr_staging_tables()
  language plpgsql
as $body$
declare
  drop_table constant text := '
    drop table if exists ? cascade;                               ';

  create_staging_table constant text := '
    create table ?(
      code         int     not null,
      geo_value    text    not null,
      signal       text    not null,
      time_value   date    not null,
      direction    text,
      issue        date    not null,
      lag          int     not null,
      value        numeric not null,
      stderr       numeric not null,
      sample_size  numeric not null,
      geo_type     text    not null,
      data_source  text    not null,
      constraint ?_pk primary key(geo_value, time_value));
    ';

  names constant text[] not null := (
    select array_agg(staging_table) from covidcast_names);
  name text not null := '';
begin
  foreach name in array names loop
    execute replace(drop_table, '?',name);

    execute replace(create_staging_table, '?',name);
  end loop;
end;
$body$;
```
