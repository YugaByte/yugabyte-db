---
title: Metabase
linkTitle: Metabase
description: Metabase
block_indexing: true
menu:
  v2.1:
    identifier: metabase
    parent: ecosystem-integrations
    weight: 576
isTocNested: true
showAsideToc: true
---

[Metabase](https://www.metabase.com/) is an extremly easy-to-use Business Intelligence (BI) tool. It bills itself as `the easy, open source way for everyone in your company to ask questions and learn from data`. This page shows how Metabase can be setup to integrate with YugabyteDB's PostgreSQL compatible API.

## 1. Start local cluster

Follow [Quick Start](../../../quick-start/) instructions to run a local YugabyteDB cluster. Test YugabyteDB's PostgreSQL compatible YSQL API as [documented](../../../quick-start/test-postgresql/) so that you can confirm that you have a PostgresSQL compatible service running on `localhost:5433`. 

## 2. Load data

### Download the sample schema

```sh
$ wget https://raw.githubusercontent.com/yugabyte/yb-sql-workshop/master/query-using-bi-tools/schema.sql
```

### Download the sample data

```sh
$ wget https://github.com/yugabyte/yb-sql-workshop/raw/master/query-using-bi-tools/sample-data.tgz
```

```sh
$ tar zxvf sample-data.tgz
```

```sh
$ ls data/
```

```
orders.sql	products.sql	reviews.sql	users.sql
```

### Connect to YugabyteDB using ysqlsh

You can do this as shown below.

```sh
$ ./bin/ysqlsh
```

```
ysqlsh (11.2)
Type "help" for help.

yugabyte=#
```

### Create a database

```postgresql
yugabyte=# CREATE DATABASE yb-demo;
```

```postgresql
yugabyte=# GRANT ALL ON DATABASE yb-demo to yugabyte;
```

```postgresql
yugabyte=# \c yb-demo;
```

### Create schema and load data

First create the 4 tables necessary to store the data.

```postgresql
yugabyte=# \i 'schema.sql';
```

Now load the data into the tables.

```postgresql
yugabyte=# \i 'data/products.sql'
```

```postgresql
yugabyte=# \i 'data/users.sql'
```

```postgresql
yugabyte=# \i 'data/orders.sql'
```

```postgresql
yugabyte=# \i 'data/reviews.sql'
```

## 3. Download and configure Metabase

Detailed steps for setting up Metabase are available [here](https://www.metabase.com/docs/latest/setting-up-metabase.html). The following are the minimal setup steps for getting started.

```sh
$ wget http://downloads.metabase.com/v0.30.4/metabase.jar
```

```sh
$ java -jar metabase.jar
```

Go to http://localhost:3000 to configure your Metabase server and point it to the YSQL API endpoint at `localhost:5433`.

## 4. Run complex queries with Metabase

Detailed steps on how to use Metabase are available [here](https://www.metabase.com/docs/latest/getting-started.html). For this doc, you will specifically focus on asking questions that require RDBMS capabilities.

- Filter data using WHERE clauses
- Join data between tables
- Perform data aggregation using GROUP BY
- Use built-in functions such as SUM, MIN, MAX, etc.

You can click on Ask a Question -> Custom Query. Choose the database you just setup, and enter the SQL queries noted in the [Retail Analytics](../../realworld-apps/retail-analytics/) section.
