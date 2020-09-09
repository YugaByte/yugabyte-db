---
title: Verify a migration from PostgreSQL
headerTitle: Verify a migration
linkTitle: Verify Migration
description: Steps for verifying that a migration from PostgreSQL to YugabyteDB was successful.
menu:
  latest:
    identifier: migrate-postgresql-verify
    parent: migrate-from-postgresql
    weight: 780
isTocNested: true
showAsideToc: true
---


Here are some things that can be verified to ensure that the migration was successful.

## Verify database objects

* Verify that all the tables and indexes have been created in YugabyteDB
* Ensure that triggers and constraints are migrated and are working as expected

## Verify row counts for tables

For tables with 1 million rows or less, run a `COUNT(*)` command to verify that the total number of rows match between the source database and YugabyteDB. This can be done as shown below using a PLPGSQL function.

**Step 1.** First create the following function to print out the number of rows in a single table.

```sql
create function 
cnt_rows(schema text, tablename text) returns integer
as
$body$
declare
  result integer;
  query varchar;
begin
  query := 'SELECT count(1) FROM ' || schema || '.' || tablename;
  execute query into result;
  return result;
end;
$body$
language plpgsql;
```

**Step 2:** Run the following command to print sizes of all tables in the database.

```sql
SELECT cnt_rows(table_schema, table_name)
    FROM information_schema.tables
    WHERE table_schema NOT IN ('pg_catalog', 'information_schema') 
    AND table_type='BASE TABLE'
    ORDER BY 3 DESC;
```

{{< note title="Note" >}}
It is currently not recommended to run a `COUNT(*)` query on large tables with more than 1 million rows. In such cases, it is recommended to use the ysql_dump tool to export the rows of the table.
{{< /note >}}

### Example

Below is an example illustrating the output of running the above on the Northwind database.

```
example=# SELECT cnt_rows(table_schema, table_name)
    FROM information_schema.tables
    WHERE table_schema NOT IN ('pg_catalog', 'information_schema') 
    AND table_type='BASE TABLE'
    ORDER BY 3 DESC;

 table_schema |       table_name       | cnt_rows
--------------+------------------------+----------
 public       | order_details          |     2155
 public       | orders                 |      830
 public       | customers              |       91
 public       | products               |       77
 public       | territories            |       53
 public       | us_states              |       51
 public       | employee_territories   |       49
 public       | suppliers              |       29
 public       | employees              |        9
 public       | categories             |        8
 public       | shippers               |        6
 public       | region                 |        4
 public       | customer_customer_demo |        0
 public       | customer_demographics  |        0
(14 rows)
```


