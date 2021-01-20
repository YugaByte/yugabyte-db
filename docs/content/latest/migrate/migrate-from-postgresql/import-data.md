
---
title: Import PostgreSQL data
headerTitle: Import PostgreSQL data
linkTitle: Import PostgreSQL data
description: Steps for importing PostgreSQL data into YugabyteDB.
menu:
  latest:
    identifier: migrate-postgresql-import-data
    parent: migrate-from-postgresql
    weight: 770
isTocNested: false
showAsideToc: true
---

The next step is to import the PostgreSQL data into YugabyteDB.

{{< note title="Note" >}}
After the data import step, remember to recreate any constraints and triggers that might have been disabled to speed up loading the data. This would ensure that the database will perform relational integrity checking for data going forward.
{{< /note >}}


## Import a database

To import an entire database from a `pg_dump` or `ysql_dump` export, use `ysqlsh`. The command should look as shown below.

```
$ ysqlsh -f <db-sql-script>
```

{{< tip title="Tip" >}}
The `ysqlsh` tool is a derivative of the PostgreSQL tool, `psql`. All `psql` commands would work in `ysqlsh`.
{{< /tip >}}


## Import a table using COPY FROM

Importing a single table (or a partial export from a table) can be done by running the COPY FROM command, and providing it the location of the export file prepared in a previous step. This should look as shown below.

```
COPY country FROM 'export.csv' DELIMITER ',' CSV HEADER;
```


