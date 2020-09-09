---
title: Migration process overview
headerTitle: Migration process overview
linkTitle: Migration process overview
description: Overview of the process for migrating data and applications from other databases to YugabyteDB.
menu:
  latest:
    identifier: migration-overview
    parent: migrate
    weight: 720
isTocNested: true
showAsideToc: true
---

This page describes the general migration process from other RDBMS to YugabyteDB (YSQL API).

The process of migrating to YugabyteDB largely looks similar to the process of migrating an application to PostgreSQL. There are some minor differences between YugabyteDB and PostgreSQL, which are covered in detail in [Migrate from PostgreSQL to YugabyteDB](../migrate-from-postgresql). The high-level process of moving from any source database to YugabyteDB is outlined below.

### Step 1. Migrate DDL (schema)

Migrate the schema from the source database to YugabyteDB. Migrating an application built for PostgreSQL to YugabyteDB should be relatively straightforward. Here is the recommended path to migrate applications to YugabyteDB.
* Convert your schema (DDL) to a PostgreSQL schema. 
* YugabyteDB schema has slight differences from that of PostgreSQL, apply these changes to the schema from the previous step. 
* If  the data set being imported is large, do not enable constraints and triggers if possible. These can be enabled at a later point.

The set of modifications to a PostgreSQL schema in order to get it to run on YugabyteDB is outlined in the section on migrating your schema from PostgreSQL to YugabyteDB.


### Step 2. Migrate DML statements
The next step is to migrate the DML statements used by the application to YugabyteDB. The support for DML in YugabyteDB is very similar to what PostgreSQL supports with some exceptions. These are outlined in the section on migrating an app from PostgreSQL to YugabyteDB.


### Step 3. Export data from source
The third step of the migration is to export the data from the various tables in the source database into a CSV format, which can subsequently be imported into YugabyteDB. Most databases support exporting data into a CSV format, which is the recommended option. How to export data from PostgreSQL into CSV is outlined here. For larger datasets, a parallel export is recommended.

Migrating to another database is a good trigger event to modify applications (in order to improve performance or add features). In such scenarios, the schema is often modified which would in turn require the data set to be transformed before loading it into YugabyteDB. This data set transformation is often accomplished by programmatically loading the data into YugabyteDB. 


### Step 4. Prepare cluster for import
Suggestions to prepare the YugabyteDB cluster for an efficient data import.


### Step 5. Import data
Import the data into YugabyteDB using the COPY command from CSV files, or programmatically.


### Step 6. Finalize DDL (schema)
If any constraints and triggers were disabled to speed up data import, recreate those.


### Step 7. Verify the migration
Check that the list of tables, indexes, constraints and triggers match between the source database and YugabyteDB.

