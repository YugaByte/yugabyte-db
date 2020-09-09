---
title: PgExercises sample database
headerTitle: PgExercises
linkTitle: PgExercises
description: Use the PgExercises sample database on YugabyteDB to learn SQL or test your knowledge.
image: /images/sample-data/pgexercises/pgsql.png
block_indexing: true
menu:
  stable:
    identifier: pgexercises
    parent: sample-data
    weight: 2753
isTocNested: true
showAsideToc: true
---

Download and install the PostgreSQL-compatible version of PgExercises on the YugabyteDB distributed SQL database. Work through 81 exercises to learn SQL or test your knowledge.

## About the PgExercises sample database

The PgExercises sample database is based on the sample dataset used for the [PostgreSQL Exercises](https://pgexercises.com/) tutorial website. The dataset is for a new country club, with a set of members, facilities, and booking history.

The PostgreSQL Exercises website includes 81 exercises designed to be used as a companion to the official PostgreSQL documentation. The exercises on the PgExercises site range from simple SELECT statements and WHERE clauses, through JOINs and CASE statements, then on to aggregations, window functions, and recursive queries.

For further details about the data, see the [PostgresSQL Exercises' Getting Started page](https://pgexercises.com/gettingstarted.html).

The `exercises` database consists of three tables (for members, bookings, and facilities) and the table relationships as shown in the entity relationship diagram.

![PgExercises ER diagram](/images/sample-data/pgexercises/pgexercises-er-diagram.png)

## Install the PgExercises sample database

Follow the steps here to download and install the PgExercises sample database.

### Before you begin

To use the PgExercises sample database, you must have installed and configured YugabyteDB. To get up and running quickly, see [Quick Start](../../quick-start/).

### 1. Download the SQL scripts

You can download the PGExercise SQL scripts that is compatible with YugabyteDB from the [`sample` directory of the YugabyteDB GitHub repository](https://github.com/yugabyte/yugabyte-db/tree/master/sample).

Here are the two files you’ll need.

- [`clubdata_ddl.sql`](https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/sample/clubdata_ddl.sql) — Creates the tables and other database objects
- [`clubdata_data.sql`](https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/sample/clubdata_data.sql) — Loads the sample data

### 2. Open the YSQL shell

To open the YSQL shell, run the `ysqlsh` command from the YugabyteDB root directory.

```sh
$ ./bin/ysqlsh
```

```
ysqlsh (11.2)
Type "help" for help.
yugabyte=#
```

### 3. Create the PgExercises database

To create the `exercises` database, run the following SQL `CREATE DATABASE` command.

```postgresql
CREATE DATABASE exercises;
```

Confirm that you have the `exercises` database by listing the databases on your cluster using the `\l` command.

```postgresql
yugabyte=# \l
```

Connect to the `exercises` database.

```postgresql
yugabyte=# \c exercises
```

```
You are now connected to database "exercises" as user "yugabyte".
exercises=#
```

### 4. Build the PgExercises tables and objects

To build the tables and database objects, run the `\i` command.

```postgresql
exercises=# \i share/clubdata_ddl.sql
```

You can verify that all three tables have been created by running the `\d` command.

```postgresql
exercises=# \d
```

### 5. Load the sample data

To load the `exercises` database with sample data, run the following command to execute commands in the file.

```postgresql
exercises=# \i share/clubdata_data.sql
```

You can verify that you have data to work with by running the following `SELECT` statement to pull data from the `bookings` table.

```postgresql
exercises=# SELECT * FROM bookings LIMIT 5;
```

## Explore the PgExercises database

You are now ready to start working through the [PostgreSQL Exercises](https://pgexercises.com/) exercises using YugabyteDB as the backend. The 81 exercises at the PostgreSQL Exercises website are broken into the following major sections.

- [Simple SQL Queries](https://pgexercises.com/questions/basic/)
- [JOINs and Subqueries](https://pgexercises.com/questions/joins/)
- [Modifying Data](https://pgexercises.com/questions/updates/)
- [Aggregation](https://pgexercises.com/questions/aggregates/)
- [Working with Timestamps](https://pgexercises.com/questions/date/)
- [String Operations](https://pgexercises.com/questions/string/)
- [Recursive Queries](https://pgexercises.com/questions/recursive/)

YugabyteDB returns the same results as expected based on the solutions on the PostgreSQL Exercises website, with the following exceptions.

- ["Find telephone numbers with parentheses"](https://pgexercises.com/questions/string/reg.html)
  - YugabyteDB returns results with a sort order of strings different than in PostgreSQL due to [hash partitioning in YugabyteDB](../../architecture/docdb/sharding/).
- ["Update a row based on the contents of another row"](https://pgexercises.com/questions/updates/updatecalculated.html)
  - YugabyteDB returns an error because using the `FROM` clause in `UPDATE` is not yet supported. See [GitHub issue #738](https://github.com/yugabyte/yugabyte-db/issues/738).
