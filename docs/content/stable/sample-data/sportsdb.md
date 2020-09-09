---
title: SportsDB sample database
headerTitle: SportsDB sample database
linkTitle: SportsDB
description: Use the SportsDB to query sports statistics while learning YugabyteDB.
block_indexing: true
menu:
  stable:
    identifier: sportsdb
    parent: sample-data
    weight: 2754
isTocNested: true
showAsideToc: true
---

If you like sports statistics, you can install the PostgreSQL-compatible version of SportsDB on the YugabyteDB distributed SQL database and explore statistics for your favorite sport.

## About the SportsDB sample database

[SportsDB](http://www.sportsdb.org/sd) is a sample sports statistics dataset compiled from multiple sources and encompassing a variety of sports, including football, baseball, basketball, ice hockey, and soccer. It also cross-references many different types of content media. It is capable of supporting queries for the most intense of sports data applications, yet is simple enough for use by those with minimal database experience. The database includes over 100 tables and just as many sequences, unique constraints, foreign keys, and indexes. The dataset also includes almost 80,000 rows of data. It has been ported to MySQL, SQL Server and PostgreSQL.

If you like details, check out this detailed entity relationship (ER) diagram.

![SportsDB ER diagram](/images/sample-data/sportsdb/sportsdb-er-diagram.jpg)

## Install the SportsDB sample database

Follow the steps here to download and install the SportsDB sample database.

### Before you begin

To use the SportsDB sample database, you must have installed and configured YugabyteDB. To get up and running quickly, see [Quick Start](/stable/quick-start/).

### 1. Download the SportsDB scripts

The SQL scripts that you need to create the SportsDB sample database (YugabyteDB-compatible) are available in the [`sample` directory of the YugabyteDB GitHub repository](https://github.com/yugabyte/yugabyte-db/tree/master/sample). Download the following five files.

- [`sportsdb_tables.sql`](https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/sample/sportsdb_tables.sql) — Creates the tables and sequences
- [`sportsdb_inserts.sql`](https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/sample/sportsdb_inserts.sql) — Loads the sample data into the `sportsdb` database
- [`sportsdb_constraints.sql`](https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/sample/sportsdb_constraints.sql) — Creates the unique constraints
- [`sportsdb_fks.sql`](https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/sample/sportsdb_fks.sql) — Creates the foreign key constraints
- [`sportsdb_indexes.sql`](https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/sample/sportsdb_indexes.sql) — Creates the indexes

### 2. Open the YSQL shell

To open the Yugabyte SQL (YSQL) shell, run the `ysqlsh` command from the YugabyteDB root directory.

```sh
$ ./bin/ysqlsh
```

```
ysqlsh (11.2)
Type "help" for help.
yugabyte=#
```

### 3. Create the SportsDB database

To create the `sportsdb` database, run the following YSQL command

```postgresql
CREATE DATABASE sportsdb;
```

Confirm that you have the `sportsdb` database by listing out the databases on your cluster.

```postgresql
yugabyte=# \l
```

Connect to the `sportsdb` database.

```postgresql
yugabyte=# \c sportsdb
```

```
You are now connected to database "sportsdb" as user "yugabyte".
sportsdb=#
```

### 4. Build the SportsDB tables and sequences

To build the tables and database objects, run the following command.

```postgresql
sportsdb=# \i share/sportsdb_tables.sql
```

You can verify that all 203 tables and sequences have been created by running the `\d` command.

```postgresql
sportsdb=# \d
```

### 5. Load sample data into the SportsDB database

To load the `sportsdb` database with sample data (~80k rows), run the following command to execute commands in the file.

```postgresql
sportsdb=# \i share/sportsdb_inserts.sql
```

To verify that you have some data to work with, you can run the following simple SELECT statement to pull data from the  basketball_defensive_stats` table.

```postgresql
sportsdb=# SELECT * FROM basketball_defensive_stats WHERE steals_total = '5';
```

### 6. Create unique constraints and foreign key

To create the unique constraints and foreign keys, run the following commands.

```postgresql
sportsdb=# \i share/sportsdb_constraints.sql
```

and

```postgresql
sportsdb=# \i share/sportsdb_fks.sql
```

### 7. Create the indexes

To create the indexes, run the following command.

```postgresql
sportsdb=# \i share/sportsdb_indexes.sql
```

## Explore the SportsDB database

That’s it! Using the command line or your favorite PostgreSQL development or administration tool, you are now ready to start exploring the SportsDB database and YugabyteDB features.
