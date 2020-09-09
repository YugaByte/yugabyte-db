---
title: Export Data
headerTitle: Export Data
linkTitle: Export Data
description: How to export data from PostgreSQL.
block_indexing: true
menu:
  stable:
    identifier: migrate-postgresql-export
    parent: migrate-from-postgresql
    weight: 750
isTocNested: false
showAsideToc: true
---

The recommended way to export data from PostgreSQL for purposes of importing it to YugabyteDB is using the CSV format.

## Exporting an entire database

The recommended way to dump an entire database from PostgreSQL is to use the YugabyteDB [`ysql_dump`](https://docs.yugabyte.com/stable/admin/ysql-dump) backup utility, which is in turn derived from PostgreSQL pg_dump.

```
$ ysql_dump -d mydatabase > mydatabase-dump.sql
```

{{< note title="Note" >}}
The `ysql_dump` may has been tested on PostgreSQL v11.2, and not work on very new versions of PostgreSQL. To export an entire database in these cases, use the pg_dump tool, this is documented in detail in the [PostgreSQL documentation on pg_dump](https://www.postgresql.org/docs/12/app-pgdump.html).
{{< /note >}}

## Export using COPY

This is an alternative to using ysql_dump in order to export a single table from the source PostgreSQL database into CSV files. This tool allows extracting a subset of rows and/or columns from a table. This can be achieved by connecting to the source DB using psql and using the `COPY TO` command, as shown below. 

```
COPY mytable TO 'export-1.csv' DELIMITER ',' CSV HEADER;
```

To extract a subset of rows from a table, it is possible to output the result of an SQL command.

```
COPY (
  SELECT * FROM mytable
    WHERE <where condition>
) TO 'export-1.csv' DELIMITER ',' CSV HEADER;
```

The various options here are described in detail in the [PostgreSQL documentation for the COPY command](https://www.postgresql.org/docs/12/sql-copy.html).

## Run large table exports in parallel

Exporting large data sets from PostgreSQL can be made efficient by running multiple COPY processes in parallel for a subset of data. This will result in multiple csv files being produced, which can subsequently be imported in parallel.

An example of running multiple exports in parallel is shown below. Remember to use a suitable value for *num_rows_per_export*, for example 1 million rows.

```
COPY (
  SELECT * FROM mytable
    ORDER BY primary_key_col 
    LIMIT num_rows_per_export OFFSET 0
) TO 'export-1.csv' DELIMITER ',' CSV HEADER;

COPY (
  SELECT * FROM mytable
    ORDER BY primary_key_col 
    LIMIT num_rows_per_export OFFSET num_rows_per_export
) TO 'export-2.csv' WITH CSV;

...
```


