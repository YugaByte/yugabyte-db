---
title: Copy the .csv files to staging tables
linkTitle: Copy the .csv files to staging tables
headerTitle: Copy each of the COVIDcast data .csv files to a dedicated staging table
description: Copy each of the COVIDcast data .csv files to a dedicated staging table for analysis using YSQL functions for linear regression analysis
menu:
  latest:
    identifier: stage-the-csv-files
    parent: ingest-the-covidcast-data
    weight: 20
isTocNested: true
showAsideToc: true
---

## First, create a dedicated ordinary user for the project

It isn't essential to do this. It's very unlikely that the names of the objects that are created for your COVIDcast case study project will collide with those of any objects that you might already have (and might care about). But the scripts that create them follow usual practice and start by dropping them. So there is a risk—at least in principle. Anyway, it makes sense to create a dedicated ordinary user for this project for all the usual reasons. For example, when all the project objects are owned by a user that owns no other objects (and are all in the same schema), it's easy to list all of the objects (and only these).

## Create and populate the "covidcast_names" table

Each of the `.csv` files that you downloaded has its unique name; each will be imported, using the `\copy` metacommand "as is" into a dedicated staging table whose name reflects the source `.csv` file; and each has its own value in the [_"signal"_](../inspect-the-csv-files/#signal) column. Because these names will be needed in various places within the overall set of scripts, they are defined in the _"covidcast_names"_ table, thus:

```plpgsql
drop table if exists covidcast_names cascade;
create table covidcast_names(csv_file text primary key, staging_table text not null, signal text not null);

-- Each of these files contains 50 days of observations.
insert into covidcast_names(csv_file, staging_table, signal) values
  ('csv-files/covidcast-fb-survey-smoothed_'|| 'wearing_mask' ||'-2020-09-13-to-2020-11-01.csv', 'mask_wearers',   'smoothed_wearing_mask'),
  ('csv-files/covidcast-fb-survey-smoothed_'|| 'cli'          ||'-2020-09-13-to-2020-11-01.csv', 'symptoms',       'smoothed_cli'),
  ('csv-files/covidcast-fb-survey-smoothed_'|| 'hh_cmnty_cli' ||'-2020-09-13-to-2020-11-01.csv', 'cmnty_symptoms', 'smoothed_hh_cmnty_cli');

create unique index covidcast_names_staging_table_unq on covidcast_names(staging_table);
create unique index covidcast_names_signal_unq on covidcast_names(signal);
```

This code is included directly in [`ingest-the-data.sql`](../ingest-scripts/ingest-the-data-sql) so you don't need to save it to a file.

## Create the three staging tables

Because the three `.csv` files have the same format as each other (see the section [Inspect the .csv files](../inspect-the-csv-files/)), each of the three staging tables has the same column names and data types. Dynamic SQL is used to create these tables to avoid repetition of code (i.e. to keep the code size as small as possible), and to bring optimal "single point of definition" maintainability.

This code is implemented by the procedure _cr_staging_tables()_ created by the [`cr-cr-staging-tables.sql`](../ingest-scripts/cr-cr-staging-tables-sql) script.

After creating the procedure, run it like this:

```plpgsql
call cr_staging_tables();
```

This invocation is included in [`ingest-the-data.sql`](../ingest-scripts/ingest-the-data-sql).

Now take an inventory of your tables with the `\d` metacommand. You should see something like this, with the names of the user and schema that you created in place of _"u1"_:

```
 Schema |      Name       | Type  | Owner 
--------+-----------------+-------+-------
 u1     | cmnty_symptoms  | table | u1
 u1     | covidcast_names | table | u1
 u1     | mask_wearers    | table | u1
 u1     | symptoms        | table | u1
```

## Use the \copy metacommand to copy the data from each .csv file into its staging table

The [`COPY`](../../../../../the-sql-language/statements/cmd_copy) SQL statement is designed to ingest data from a file "as is". However, its simple use requires that the to-be-read (or to-be-written) file resides _server-side_ on the local filesystem of the YB-TServer that you connect to. If you specify `stdin` as the argument of `COPY FROM`, then these input and output channels are defined client-side in the environment of the client where you run `ysqlsh`. This sounds promising. But the snag is that you must include the text of the `COPY FROM` statement at the start of the file that contains the data that you intend to ingest. This is described in the [stdin and stdout](../../../../../the-sql-language/statements/cmd_copy/#stdin-and-stdout) section of the documentation for the `COPY` statement.

The preferred option for the present case study, because [`ysqlsh`](../../../../../../../admin/ysqlsh/) is chosen for running all the SQL statements, is to use the [`\copy`](../../../../../../../admin/ysqlsh/#copy-table-column-list-query-from-to-filename-program-command-stdin-stdout-pstdin-pstdout-with-option) metacommand.

Because the three `.csv` files all have the same format, as do their three dedicated staging tables, the `copy` command will have the same form for each of its invocations, thus:

```
\copy <staging table> from <csv file> with (format 'csv', header true)
```

This is another case where a procedure that reads the names from the _"covidcast_names"_ table avoids repetition of code and brings optimal "single point of definition" maintainability. However, there's particular design dilemma to confront. The `\copy` metacommand cannot be run from a stored procedure written in PL/pgSQL. Therefore a _function_ is used that will return the text of the `\copy` metacommand. You execute the appropriately parameterized function, spool its output to a `.sql` script and start that script.

- Create the function with the [`cr-cr-copy-from-csv-scripts.sql`](../ingest-scripts/cr-cr-copy-from-csv-scripts-sql) script.

- Test it manually like this:

   ```plpgsql
   \t on
   select cr_copy_from_scripts(1);
   \t off
   ```

- This is the result:

   ```
   \copy symptoms from 'csv-files/covidcast-fb-survey-smoothed_cli-2020-09-13-to-2020-11-01.csv' with (format 'csv', header true);
   ```

- Execute it for each to-be-ingested `.csv` file.

   ```plpgsql
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
   ```
   
   This code is included directly in [`ingest-the-data.sql`](../ingest-scripts/ingest-the-data-sql) so you don't need to save it to a file.

This pattern—using a PL/pgSQL function to create a `sql` script whose details reflect the present content of database tables and then executing it from `ysqlsh` is a useful generic technique.

Finally, check visually that each staging table has the same number of rows:

```plpgsql
select
  (select count(*) from mask_wearers)  as "mask_wearers count",
  (select count(*) from symptoms)       as "symptoms count",
  (select count(*) from cmnty_symptoms) as "cmnty_symptoms count";
```

This is the result:

```
 mask_wearers count | symptoms count | cmnty_symptoms count 
--------------------+----------------+----------------------
               2550 |           2550 |                 2550
```

The value _2,550_ is the product of the _51_ states (including DC) and the _50_ days on which survey data were collected.
